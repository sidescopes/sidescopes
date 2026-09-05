// Browser Lab host for the shared scope pipeline and pane UI. The host
// supplies local images and a virtual display region in place of native
// capture and desktop overlays. Loaded image pixels remain in the browser.

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/emscripten_glfw3.h>
#include <GLFW/glfw3.h>

#include "app/app_startup.h"
#include "app/attach_controller.h"
#include "app/capture_controller.h"
#include "app/context_menu.h"
#include "app/guided_tour.h"
#include "app/imgui_context_menu.h"
#include "app/layout_preset_picker.h"
#include "app/layout_preset_store.h"
#include "app/layout_presets.h"
#include "app/pane_render.h"
#include "app/pin_board.h"
#include "app/preferences_binding.h"
#include "app/region_picker.h"
#include "app/scope_pane_renderer.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"
#include "app/tour_overlay.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/preferences.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/desktop.h"
#include "platform/graphics.h"
#include "platform/screen_capture.h"
#include "platform/web/screen_capture_source.h"
#include "web/image_adjust.h"
#include "web/lab_layout.h"
#include "web/lab_picture.h"
#include "web/lab_shell.h"
#include "web/lab_storage.h"
#include "web/lab_tour_steps.h"
#include "web/region_editor.h"

namespace sidescopes {
namespace {

/// Everything the lab owns, in one place so the animation-frame callback
/// can reach it without a pile of globals.
struct Lab
{
    GLFWwindow* window = nullptr;
    std::unique_ptr<GraphicsBackend> graphics;

    std::unique_ptr<ScopeRegistry> registry;
    std::unique_ptr<ScopeView> view;
    std::unique_ptr<PinBoard> pins;

    FrameMailbox mailbox;
    std::unique_ptr<ScreenCaptureSource> capture;
    std::unique_ptr<CaptureController> captureController;
    /// The real one, pumped from the frame loop rather than given a thread.
    std::unique_ptr<AnalysisWorker> worker;
    std::unique_ptr<RegionPicker> picker;
    std::unique_ptr<ShortcutResolver> shortcuts;

    AnalysisSettings analysis;
    AnalysisWorker::Output output;

    /// How far into the worker's output this host has read, as every host
    /// of the worker keeps.
    uint64_t outputVersion = 0;

    std::unique_ptr<ScopePaneRenderer> panes;

    /// The photograph, in the three forms it has to exist in: what the page
    /// decoded, what the engines read, and what the canvas draws.
    LabPicture picture;
    /// The texture the display copy is uploaded to. The picture holds no
    /// texture and knows no backend, which is what keeps it testable; owning
    /// the graphics is this shell's job.
    std::unique_ptr<ScopeTexture> displayTexture;

    /// The ANALYSIS SETTINGS need sending again - a region moved, a scope
    /// appeared. Nothing to do with the picture's pixels, which the picture
    /// tracks itself.
    bool settingsDirty = false;

    /// No window can be attached in a page; the menu reads the empty state.
    AttachController attach;
    /// The layout chip and its save button: the middle of the application's
    /// toolbar, and without it the row reads as two stray icons.
    std::unique_ptr<LayoutPresetController> presetController;
    std::unique_ptr<LayoutPresetPicker> presetPicker;
    /// The parameter entries the open menu was built with; menuScopeParam
    /// resolves a chosen id against them, so they outlive the build.
    std::vector<ParamMenuAction> menuParams;

    /// The walk-through, and where each control it names landed this frame.
    std::unique_ptr<GuidedTour> tour;
    TourAnchors anchors;
    /// The anchors the PAGE owns, by tour id, because the filmstrip and the
    /// adjustment controls are the document's rather than the application's.
    std::map<std::string, ImVec4> pageAnchors;
    /// What the preferences remembered, until the tour exists to be told.
    bool tourSettled = false;

    RegionEditor region;
    /// The pin tool, armed by P or its button. It needs no desktop at all -
    /// it samples the picture - so it is answered here rather than refused.
    bool pinArmed = false;
    bool pinning = false;
    ImVec2 pinFrom{0.0f, 0.0f};
    /// The supplied image and the Lab's virtual display have separate
    /// coordinate systems. The global region belongs to the latter and keeps
    /// its position when the image beneath it changes.
    RegionEditor::Placement picturePlacement;
    RegionEditor::Placement displayPlacement;
    /// The Lab starter policy needs geometry from the first layout pass.
    bool starterRegionDue = true;
    /// The three colours the pane input distinguishes, and they are not the
    /// same value. The READOUT follows the cursor wherever it is - on the
    /// desktop that is always over pixels, and here the picture is what
    /// there is, so the last sample is held rather than blanked the moment
    /// the pointer steps onto the application's own window. The TRACE
    /// markers are empty whenever the pointer is outside the region the
    /// scopes actually read, which is what the marker means.
    std::optional<FloatColor> readoutColour;
    std::optional<FloatColor> traceColour;
    ImFont* monospaceFont = nullptr;
    /// Set when something worth remembering changed. Written at the end of
    /// the frame rather than at the moment of the change, so a gesture that
    /// reports several does not write several times.
    bool saveDue = false;
};

Lab g_lab;

/// Uploads the picture's display copy to its texture; defined with the graphics
/// work further down, needed by the adjustment pass just below.
void refreshDisplayTexture();

/// Captures the selected part of the Lab's virtual display. The photograph is
/// only one window on that display: any selected area around it remains the
/// black desktop the visitor can see, exactly as an operating-system screen
/// capture would report it to the desktop application.
LabDisplayCapture captureSelectedDisplay()
{
    if (!g_lab.region.hasRegion() || g_lab.picture.width() <= 0 || g_lab.picture.height() <= 0) {
        return {};
    }
    const SsRect rect = g_lab.region.rect();
    const LayoutRect regionOnDisplay{
        LayoutPoint{g_lab.displayPlacement.origin.x + static_cast<float>(rect.x) * g_lab.displayPlacement.scale,
                    g_lab.displayPlacement.origin.y + static_cast<float>(rect.y) * g_lab.displayPlacement.scale},
        LayoutPoint{static_cast<float>(rect.width) * g_lab.displayPlacement.scale,
                    static_cast<float>(rect.height) * g_lab.displayPlacement.scale}};
    const LayoutRect pictureOnDisplay{
        LayoutPoint{g_lab.picturePlacement.origin.x, g_lab.picturePlacement.origin.y},
        LayoutPoint{static_cast<float>(g_lab.picture.display().width) * g_lab.picturePlacement.scale,
                    static_cast<float>(g_lab.picture.display().height) * g_lab.picturePlacement.scale}};
    return captureVirtualDisplayRegion(
        regionOnDisplay, pictureOnDisplay,
        LayoutPoint{static_cast<float>(g_lab.picture.width()), static_cast<float>(g_lab.picture.height())},
        g_lab.picture.analysed());
}

/// One turn of the analysis, through the REAL pipeline.
///
/// The picture goes into the capture source, which publishes it to the
/// mailbox; the worker takes it from there and runs the same pass the desktop
/// runs. What was here before was a private copy of that pass - it configured
/// the instances, accumulated, and assembled an Output by hand - and a private
/// copy is free to drift from the one that ships.
void analyse()
{
    if (g_lab.picture.refresh()) {
        refreshDisplayTexture();
    }
    // Moving or resizing a GLOBAL region changes the captured frame even when
    // the photograph itself did not change, because a different share of the
    // surrounding virtual desktop may now be included.
    if ((g_lab.picture.hasFreshPixels() || g_lab.settingsDirty) && g_lab.region.hasRegion()) {
        const LabDisplayCapture captured = captureSelectedDisplay();
        submitCapturedPicture(captured.bgra.data(), captured.width, captured.height);
        g_lab.picture.pixelsTaken();
    }
    if (g_lab.settingsDirty) {
        // The frame submitted above is already the selected desktop region,
        // so the real analysis pipeline reads all of it. No selected region
        // still means no measurement, exactly as it does on the desktop.
        g_lab.analysis.region =
            g_lab.region.hasRegion() ? std::optional<RegionOfInterest>{RegionOfInterest{}} : std::nullopt;
        g_lab.worker->updateSettings(g_lab.analysis);
        g_lab.settingsDirty = false;
    }
    // The worker has no thread of its own here, so its pass runs on this one.
    g_lab.worker->pump();
    (void)g_lab.worker->fetchOutput(g_lab.outputVersion, g_lab.output);
}

/// Fits the picture into @p area, centred, never enlarged past its own
/// pixels — a photograph blown up would invite reading detail that is not
/// there.
[[nodiscard]] RegionEditor::Placement placePicture(const ImVec2& cursor, const ImVec2& area)
{
    const float wide = area.x / static_cast<float>(std::max(1, g_lab.picture.display().width));
    const float tall = area.y / static_cast<float>(std::max(1, g_lab.picture.display().height));
    const float scale = std::min({wide, tall, 1.0f});
    const float width = static_cast<float>(g_lab.picture.display().width) * scale;
    const float height = static_cast<float>(g_lab.picture.display().height) * scale;

    return RegionEditor::Placement{ImVec2{cursor.x + (area.x - width) * 0.5f, cursor.y + (area.y - height) * 0.5f},
                                   scale};
}

/// The pin gesture: click takes the colour under the cursor, drag takes the
/// average of the rectangle. Dragging matters more than it sounds - one
/// pixel of skin, sky or fabric is very often unrepresentative, which is
/// why the application offers both and so does this.
///
/// @return Whether the pin tool consumed the gesture, so the region editor
///         leaves the pointer alone this frame.
[[nodiscard]] bool runPinTool(const RegionEditor::Placement& placement)
{
    if (!g_lab.pinArmed) {
        return false;
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
        g_lab.pinning = true;
        g_lab.pinFrom = mouse;
    }
    if (!g_lab.pinning) {
        return true;
    }
    const float scale = placement.scale > 0.0f ? placement.scale : 1.0f;
    const ImVec2 from{std::min(g_lab.pinFrom.x, mouse.x), std::min(g_lab.pinFrom.y, mouse.y)};
    const ImVec2 to{std::max(g_lab.pinFrom.x, mouse.x), std::max(g_lab.pinFrom.y, mouse.y)};
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (to.x - from.x > 2.0f || to.y - from.y > 2.0f)) {
        ImGui::GetWindowDrawList()->AddRect(from, to, IM_COL32(255, 255, 255, 200));
    }
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        return true;
    }
    g_lab.pinning = false;
    const SsRect area{static_cast<int>((from.x - placement.origin.x) / scale),
                      static_cast<int>((from.y - placement.origin.y) / scale),
                      std::max(1, static_cast<int>((to.x - from.x) / scale)),
                      std::max(1, static_cast<int>((to.y - from.y) / scale))};
    const std::optional<FloatColor> colour = shell::averageOver(
        area, g_lab.picture.display().rgba, g_lab.picture.display().width, g_lab.picture.display().height);
    if (colour.has_value()) {
        g_lab.pins->pin(*colour);
        g_lab.saveDue = true;
    }
    // Shift keeps the tool up to pin several, as the application does.
    g_lab.pinArmed = shell::modifiers().shift;
    g_lab.panes->setStatus(g_lab.pinArmed ? "Pinned - keep pinning, or release Shift"
                                          : (colour.has_value() ? "Color pinned" : "Nothing under the pointer"));

    return true;
}

/// Applies the Lab starter-region policy once the virtual display is laid out.
[[nodiscard]] bool initializeStarterRegion(const ImVec2& position, const ImVec2& area, const ShellLayout& layout)
{
    if (!g_lab.starterRegionDue) {
        return false;
    }
    const LayoutRect starter = starterRegionFor(layout);
    RegionOfInterest region;
    if (area.x > 0.0f && area.y > 0.0f) {
        region.leftPercent = (starter.position.x - position.x) / area.x * 100.0;
        region.topPercent = (starter.position.y - position.y) / area.y * 100.0;
        region.rightPercent = (starter.position.x + starter.size.x - position.x) / area.x * 100.0;
        region.bottomPercent = (starter.position.y + starter.size.y - position.y) / area.y * 100.0;
    }
    g_lab.region.reset(region, static_cast<int>(std::lround(area.x)), static_cast<int>(std::lround(area.y)));
    g_lab.starterRegionDue = false;

    return true;
}

/// The Lab's virtual display: the supplied picture beneath a global region.
/// The application window is drawn afterwards so it stays above the region,
/// just as it does on the desktop. @return Whether the region moved.
[[nodiscard]] bool drawDisplay(const ImVec2& position, const ImVec2& area, const ShellLayout& layout)
{
    bool moved = false;
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(area);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    // No frame of our own: on a desktop the window's edge belongs to the
    // operating system, and a rounded ImGui border here reads as a seam.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##screen", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    if (g_lab.displayTexture != nullptr && g_lab.picture.display().width > 0) {
        g_lab.displayPlacement = RegionEditor::Placement{position, 1.0f};
        g_lab.picturePlacement = placePicture(ImVec2{layout.screenPos.x, layout.screenPos.y},
                                              ImVec2{layout.screenSize.x, layout.screenSize.y});
        const ImVec2 size{static_cast<float>(g_lab.picture.display().width) * g_lab.picturePlacement.scale,
                          static_cast<float>(g_lab.picture.display().height) * g_lab.picturePlacement.scale};
        ImGui::GetWindowDrawList()->AddImage(
            g_lab.displayTexture->textureId(), g_lab.picturePlacement.origin,
            ImVec2{g_lab.picturePlacement.origin.x + size.x, g_lab.picturePlacement.origin.y + size.y});
        moved = initializeStarterRegion(position, area, layout);
        if (!runPinTool(g_lab.picturePlacement)) {
            moved = g_lab.region.update(g_lab.displayPlacement, static_cast<int>(std::lround(area.x)),
                                        static_cast<int>(std::lround(area.y))) ||
                    moved;
        }
        // The desktop samples the screen under the pointer; the only picture
        // here is this one, so it is sampled the same way.
        const std::optional<FloatColor> live =
            shell::sampleAt(ImGui::GetMousePos(), g_lab.picturePlacement, g_lab.picture.display().rgba,
                            g_lab.picture.display().width, g_lab.picture.display().height);
        if (live.has_value()) {
            g_lab.readoutColour = live;
        }
        // The trace markers mean "this colour, in the region being measured",
        // so they go quiet the moment the pointer leaves it - exactly as they
        // do on the desktop.
        const SsRect region = g_lab.region.rect();
        const float scale = g_lab.displayPlacement.scale > 0.0f ? g_lab.displayPlacement.scale : 1.0f;
        const ImVec2 mouse = ImGui::GetMousePos();
        const int displayX = static_cast<int>((mouse.x - g_lab.displayPlacement.origin.x) / scale);
        const int displayY = static_cast<int>((mouse.y - g_lab.displayPlacement.origin.y) / scale);
        const bool inRegion = g_lab.region.hasRegion() && displayX >= region.x && displayY >= region.y &&
                              displayX < region.x + region.width && displayY < region.y + region.height;
        g_lab.traceColour = inRegion ? live : std::nullopt;
    } else {
        ImGui::TextDisabled("Choose a picture to measure.");
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    return moved;
}

/// What a click on a chip or a tool decided, carried out. The host does this
/// on the desktop too; only the region actions differ, because a region here
/// is a rectangle on the Lab's virtual display rather than a native window.
void applyOutcome(const PaneRenderOutcome& outcome)
{
    if (outcome.chosenScope.has_value()) {
        const ScopeChoice& choice = *outcome.chosenScope;
        (void)g_lab.view->stack().choose(choice.id, choice.stack);
        g_lab.analysis.enabledScopes = g_lab.view->stack().ids();
        g_lab.settingsDirty = true;
    }
    if (outcome.clearRegion) {
        // The desktop's own answer: the region goes, and the scopes read
        // nothing. An empty scope is a state rather than a failure, and a
        // lab that quietly selected the whole picture instead would be
        // teaching something the application does not do.
        g_lab.region.clear();
        g_lab.panes->releaseTraces();
        g_lab.settingsDirty = true;
    }
    if (outcome.analysisDirty) {
        g_lab.panes->configureProjections();
        g_lab.settingsDirty = true;
    }
    if (outcome.chosenScope.has_value() || outcome.analysisDirty || outcome.preferencesSaveDue) {
        g_lab.saveDue = true;
    }
}

/// A preset outcome, applied. The controller does the work; the host pushes
/// what changed, exactly as the desktop one does.
void applyPreset(const LayoutPresetOutcome& outcome)
{
    if (!outcome.status.empty()) {
        g_lab.panes->setStatus(outcome.status);
    }
    if (outcome.analysisDirty) {
        g_lab.analysis.enabledScopes = g_lab.view->stack().ids();
        g_lab.panes->configureProjections();
        g_lab.settingsDirty = true;
    }
    g_lab.saveDue = g_lab.saveDue || outcome.preferencesSaveDue;
}

void cancelRegionInteraction()
{
    if (g_lab.pinArmed) {
        g_lab.pinArmed = false;
        g_lab.pinning = false;
        g_lab.panes->setStatus("Pinning cancelled");
        return;
    }
    PaneRenderOutcome outcome;
    outcome.clearRegion = true;
    applyOutcome(outcome);
}

void applyShortcut(const ShortcutAction& action)
{
    PaneRenderOutcome outcome;
    switch (action.kind) {
    case ShortcutAction::Kind::ChooseScope:
        outcome.chosenScope = ScopeChoice{action.scopeId, action.stack};
        applyOutcome(outcome);
        break;
    case ShortcutAction::Kind::SetZoom:
        g_lab.view->setZoom(action.zoomLevel);
        g_lab.settingsDirty = true;
        g_lab.saveDue = true;
        break;
    case ShortcutAction::Kind::ClearRegion:
        cancelRegionInteraction();
        break;
    case ShortcutAction::Kind::RequestPick:
        // Through the picker, not straight to the tool: the toolbar
        // buttons raise their requests the same way, so the keyboard and
        // the buttons meet at one place instead of two.
        g_lab.picker->request(action.pickMode);
        break;
    case ShortcutAction::Kind::LoadPreset:
        applyPreset(g_lab.presetController->load(action.presetSlot));
        break;
    case ShortcutAction::Kind::CopyPresetTo:
    case ShortcutAction::Kind::SaveActivePreset:
        applyPreset(g_lab.presetController->saveInto(action.kind == ShortcutAction::Kind::CopyPresetTo
                                                         ? action.presetSlot
                                                         : g_lab.presetController->activeSlot()));
        break;
    // Exhaustive so newly resolved actions cannot be silently dropped.
    // Window chords belong to the browser and are disabled by its platform.
    case ShortcutAction::Kind::HideApplication:
    case ShortcutAction::Kind::MinimizeWindow:
    case ShortcutAction::Kind::QuitWindow:
        break;
    // The settings window is not built in the lab, so there is nothing to
    // open and nothing to close.
    case ShortcutAction::Kind::OpenSettings:
    case ShortcutAction::Kind::CloseSettings:
        break;
    case ShortcutAction::Kind::None:
        break;
    }
}

/// The keyboard, through the application's own resolver: this decides only
/// whether a key is down, never what it means.
void applyShortcuts()
{
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    const ShortcutContext context =
        shortcutContextFor(*g_lab.view, *g_lab.registry, /*settingsOpen=*/false, /*wantsTextInput=*/false);
    for (const ShortcutAction& action :
         g_lab.shortcuts->resolvePressed(context, shell::modifiers(), shell::keyPressed)) {
        applyShortcut(action);
    }
}

/// The right-click menu. The native one is unavailable in a page, so the
/// application draws the same declarative items itself - the fallback the
/// Linux port added for the same reason, reused rather than rewritten.
void drawContextMenu(int clickedPane, bool overApplication)
{
    if (overApplication && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !nativeContextMenuAvailable()) {
        const ContextMenuModel model{
            *g_lab.view,
            *g_lab.registry,
            *g_lab.shortcuts,
            g_lab.analysis.scopeParams,
            g_lab.attach,
            g_lab.presetController->all(),
            g_lab.pins->empty(),
            g_lab.presetController->activeSlot(),
            1.0f,
            QualityLevel::Standard,
            g_lab.region.hasRegion(),
            /*applicationControlsAvailable=*/false,
        };
        std::vector<NativeMenuItem> items;
        g_lab.menuParams.clear();
        buildContextMenu(model, clickedPane, items, g_lab.menuParams);
        openImGuiContextMenu(std::move(items));
    }

    const ImGuiContextMenuFrame menu = drawImGuiContextMenu();
    if (menu.chosen < 0) {
        return;
    }
    if (const std::optional<std::string> scope = menuScopeToggle(menu.chosen, *g_lab.registry)) {
        PaneRenderOutcome outcome;
        outcome.chosenScope = ScopeChoice{*scope, /*stack=*/true};
        applyOutcome(outcome);
    } else if (const ParamMenuAction* param = menuScopeParam(menu.chosen, g_lab.menuParams)) {
        g_lab.analysis.scopeParams[param->scopeId][param->paramKey] = param->value;
        g_lab.panes->configureProjections();
        g_lab.settingsDirty = true;
    } else if (const std::optional<float> strength = menuGraticuleStrength(menu.chosen)) {
        g_lab.view->setGraticuleStrength(*strength);
    } else if (const std::optional<ShortcutAction> action = menuShortcutAction(menu.chosen)) {
        applyShortcut(*action);
    }
    if (const std::optional<LayoutOrientation> orientation = menuOrientation(menu.chosen)) {
        g_lab.view->layout().setOrientation(*orientation);
        g_lab.settingsDirty = true;
    }
    if (menu.chosen == MenuClearPinnedMarkers) {
        g_lab.pins->clear();
    }
    g_lab.saveDue = true;
}

void drawShell();
void noteControlAnchor(const char* id, const std::optional<ImVec4>& bounds);
void savePreferencesNow();
void restorePreferencesNow();

void frame()
{
    glfwPollEvents();

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(g_lab.window, &width, &height);
    if (width <= 0 || height <= 0 || !g_lab.graphics->beginFrame(width, height)) {
        return;
    }

    ImGui::NewFrame();
    drawShell();
    ImGui::Render();
    g_lab.graphics->endFrame();
}

/// The application's window: the toolbar, the panes and the status strip,
/// drawn as its own window rather than as a band of the page - because on a
/// desktop that is exactly what it is.
void drawAppWindow(const ShellLayout& layout, const PaneRenderInput& input)
{
    ImGui::SetNextWindowPos(ImVec2{layout.appPos.x, layout.appPos.y});
    ImGui::SetNextWindowSize(ImVec2{layout.appSize.x, layout.appSize.y});
    // The display window is created first and cannot bring itself to the
    // front, so this later window remains above it without taking focus away
    // from a region gesture on every frame.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##app", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    const float titleBarHeight =
        shell::drawWindowChrome(ImVec2{layout.appPos.x, layout.appPos.y}, ImVec2{layout.appSize.x, layout.appSize.y});
    ImGui::Dummy(ImVec2{0.0f, titleBarHeight});

    applyShortcuts();
    // Each toolbar group is wrapped so the walk-through can point at it: a
    // group's bounds are what GetItemRect reports once it ends, which is what
    // groups are for, and it means the tour follows the controls rather than
    // coordinates written down once and left to rot.
    ImGui::BeginGroup();
    applyOutcome(g_lab.panes->drawScopeToggles());
    ImGui::EndGroup();
    g_lab.anchors.note("chooser", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    // The preset chip sits between the scope selector and the region tools,
    // as it does on the desktop.
    applyPreset(g_lab.presetPicker->draw(g_lab.panes->icons()));
    ImGui::SameLine();
    applyOutcome(g_lab.panes->drawRegionToolIcons(input));

    ImGui::BeginGroup();
    applyOutcome(g_lab.panes->drawScopePanes(input));
    ImGui::EndGroup();
    g_lab.anchors.note("scopes", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    g_lab.panes->drawStatusBar(input);
    // Asked of the controls themselves. The region tools are right-aligned
    // and the pin sits at the other end of the application, so a group around
    // either call would frame most of the window instead of two buttons.
    noteControlAnchor("tools", g_lab.panes->regionToolBounds());
    noteControlAnchor("pin", g_lab.panes->pinToolBounds());
    for (const auto& [id, bounds] : g_lab.pageAnchors) {
        g_lab.anchors.note(id, ImVec2{bounds.x, bounds.y}, ImVec2{bounds.z, bounds.w});
    }
    // Only over the application. Right-clicking the picture is the
    // workspace's business on a desktop - there it would raise the editor's
    // menu, not this one - so it raises nothing here.
    drawContextMenu(g_lab.panes->paneAt(ImGui::GetMousePos()), ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));

    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// A region tool was asked for. The desktop opens a picker over the whole
/// desktop; there is none here, so the draw tool arms the gesture on the
/// picture and the two that need a desktop say plainly that they cannot.
void answerPickRequest()
{
    const std::optional<RegionPickerMode> want = g_lab.picker->pendingRequest();
    if (!want.has_value()) {
        return;
    }
    g_lab.picker->clearRequest();
    if (*want == RegionPickerMode::PinColor) {
        // Pinning needs no desktop: it samples the picture, which is right
        // here. Refusing it was simply wrong.
        if (g_lab.region.armed()) {
            g_lab.region.clear();
            g_lab.settingsDirty = true;
        }
        g_lab.pinning = false;
        g_lab.pinArmed = true;
        g_lab.panes->setStatus("Click a color to pin it, or drag to pin an area's average");

        return;
    }
    if (*want == RegionPickerMode::DrawGlobal) {
        g_lab.pinArmed = false;
        g_lab.pinning = false;
        g_lab.region.armDraw();
        g_lab.panes->releaseTraces();
        g_lab.settingsDirty = true;
        g_lab.panes->setStatus("Drag on the display to draw a region");

        return;
    }
    g_lab.panes->setStatus("Attaching to a window or a face needs the desktop application");
}

/// Notes a control's own rectangle, when it has drawn at least once.
void noteControlAnchor(const char* id, const std::optional<ImVec4>& bounds)
{
    if (!bounds) {
        return;
    }
    g_lab.anchors.note(id, ImVec2{bounds->x, bounds->y}, ImVec2{bounds->z, bounds->w});
}

/// Where the picture and the global region landed, for the walk-through to point at.
/// These are the two the lab owns; the toolbar and the panes note their own
/// as they draw, which is what keeps the tour pointing at controls rather
/// than at coordinates written down once and left to rot.
void notePictureAnchors()
{
    const RegionEditor::Placement& picture = g_lab.picturePlacement;
    g_lab.anchors.note("picture", picture.origin,
                       ImVec2{picture.origin.x + static_cast<float>(g_lab.picture.display().width) * picture.scale,
                              picture.origin.y + static_cast<float>(g_lab.picture.display().height) * picture.scale});
    if (!g_lab.region.hasRegion()) {
        return;
    }
    const SsRect rect = g_lab.region.rect();
    const RegionEditor::Placement& display = g_lab.displayPlacement;
    const ImVec2 topLeft{display.origin.x + static_cast<float>(rect.x) * display.scale,
                         display.origin.y + static_cast<float>(rect.y) * display.scale};
    g_lab.anchors.note("region", topLeft,
                       ImVec2{topLeft.x + static_cast<float>(rect.width) * display.scale,
                              topLeft.y + static_cast<float>(rect.height) * display.scale});
}

/// The walk-through, drawn last so its veil covers what it is talking about,
/// with its outcome applied in one place as every other outcome here is.
void runTour(const ImVec2& shellPos, const ImVec2& shellSize)
{
    const ImVec2 shellMax{shellPos.x + shellSize.x, shellPos.y + shellSize.y};
    switch (drawTourOverlay(*g_lab.tour, g_lab.anchors, shellPos, shellMax)) {
    case TourAction::Advance:
        g_lab.tour->advance();
        // Settling is worth remembering; which step was reached is not.
        g_lab.saveDue = g_lab.saveDue || g_lab.tour->settled();
        break;
    case TourAction::Skip:
        g_lab.tour->skip();
        g_lab.saveDue = true;
        break;
    case TourAction::None:
        break;
    }
}

void drawShell()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ShellLayout layout = layoutFor(LayoutPoint{viewport->WorkPos.x, viewport->WorkPos.y},
                                         LayoutPoint{viewport->WorkSize.x, viewport->WorkSize.y});

    // The pointer colour is last frame's: it is computed while the picture
    // draws, and the toolbar's own tools want an input too. One frame of lag
    // on a value the application already smooths is not a lag anyone sees.
    const PaneRenderInput input{
        /*uiScale=*/1.0f,
        /*regionSelected=*/g_lab.region.hasRegion(),
        /*pinsAvailable=*/true,
        /*vectorscopeColor=*/g_lab.traceColour,
        /*waveformColor=*/g_lab.traceColour,
        /*readoutColor=*/g_lab.readoutColour,
        /*monospaceFont=*/g_lab.monospaceFont,
    };

    // Dropped every frame, so a control that stops drawing stops being
    // pointed at rather than leaving the tour aimed at where it used to be.
    g_lab.anchors.clear();

    if (drawDisplay(viewport->WorkPos, viewport->WorkSize, layout)) {
        g_lab.settingsDirty = true;
    }
    notePictureAnchors();
    // The border's own close badge dismisses the region, and it means the
    // same thing the toolbar's clear does - so it goes through the same path.
    if (g_lab.region.takeDismissed()) {
        PaneRenderOutcome dismissed;
        dismissed.clearRegion = true;
        applyOutcome(dismissed);
    }
    analyse();
    g_lab.panes->uploadVisibleScopes(g_lab.region.hasRegion());

    drawAppWindow(layout, input);
    answerPickRequest();

    // The region tools want a crosshair, which Dear ImGui's cursor enum does
    // not carry, so the canvas is told directly. Handing it back to Dear
    // ImGui the moment neither tool is up keeps the resize cursors working.
    if (g_lab.pinArmed) {
        // Crosshair AND swatch in the cursor image, so neither can trail the
        // pointer - the desktop's construction, for the same reason.
        shell::setPinCursor(g_lab.readoutColour);
    } else if (g_lab.region.armed()) {
        shell::setCanvasCursor("crosshair");
    } else {
        shell::setCanvasCursor(nullptr);
    }

    runTour(viewport->WorkPos, viewport->WorkSize);

    if (g_lab.saveDue) {
        savePreferencesNow();
        g_lab.saveDue = false;
    }
}

/// Reads the running session back out and keeps it for next time. The
/// application's own capture does the translating; the shell adds what it
/// alone holds, which here is the preset slots.
void savePreferencesNow()
{
    Preferences saved = capturePreferences(*g_lab.view, *g_lab.pins, *g_lab.shortcuts, g_lab.analysis);
    saved.layoutPresets = g_lab.presetController->all();
    saved.layoutActiveSlot = g_lab.presetController->activeSlot();
    saved.tourSettled = g_lab.tour->settled() ? 1 : 0;
    storage::save(saved);
}

/// Puts a previous visit back, if this browser holds one. Whatever is
/// missing or malformed simply defaults - the application already tolerates
/// an old or hand-edited file, and a lab that refused to start over a
/// preference would be worse than one that forgets.
void restorePreferencesNow()
{
    const std::optional<Preferences> previous = storage::load();
    if (!previous) {
        g_lab.tourSettled = false;
        return;
    }
    const Preferences& saved = *previous;
    restorePreferences(saved, *g_lab.view, *g_lab.pins, *g_lab.shortcuts, g_lab.analysis);
    g_lab.presetController->restore(saved.layoutPresets, saved.layoutActiveSlot);
    // Applied by the caller once the tour exists: it is built after this, so
    // its text can quote the bindings this just restored.
    g_lab.tourSettled = saved.tourSettled != 0;
    g_lab.analysis.enabledScopes = g_lab.view->stack().ids();
}

/// Everything downstream of the window: the registry, the view, the capture
/// seams and the pane renderer. Split out of main so that neither runs past
/// what one screen holds.

void buildScopes()
{
    // builtinModules() is the registry the desktop build uses too: the
    // module entries are linked in and register themselves.
    g_lab.registry = std::make_unique<ScopeRegistry>(builtinModules());
    g_lab.view = std::make_unique<ScopeView>(*g_lab.registry);
    g_lab.pins = std::make_unique<PinBoard>();
    g_lab.capture = createScreenCaptureSource();
    g_lab.captureController = std::make_unique<CaptureController>(*g_lab.capture, g_lab.mailbox);
    (void)g_lab.captureController->requestPermission();
    // The picture arrives through the capture source like any other frame, so
    // the stream is genuinely running - the target is the page rather than a
    // display, and the host feeds it.
    (void)g_lab.captureController->start();

    seedImageSizes(g_lab.analysis);

    // The REAL worker, running its passes on this thread. A page has no
    // threads to give, so it is pumped from the frame loop rather than
    // started on its own; the passes are the same passes the desktop runs.
    g_lab.worker = std::make_unique<AnalysisWorker>(g_lab.mailbox);
    g_lab.worker->startInline();
    g_lab.picker = std::make_unique<RegionPicker>(*g_lab.captureController, *g_lab.worker, *g_lab.capture);
    g_lab.shortcuts = std::make_unique<ShortcutResolver>(*g_lab.registry);
    g_lab.presetController = std::make_unique<LayoutPresetController>(*g_lab.view, *g_lab.registry, g_lab.analysis);
    g_lab.presetPicker = std::make_unique<LayoutPresetPicker>(*g_lab.presetController);

    const ScopePaneContext context{
        *g_lab.graphics,          *g_lab.view,   *g_lab.registry, g_lab.analysis,   g_lab.output,
        *g_lab.captureController, *g_lab.picker, *g_lab.pins,     *g_lab.shortcuts,
    };
    g_lab.panes = std::make_unique<ScopePaneRenderer>(context, createProjectionInstances(*g_lab.registry),
                                                      createScopeTextures(*g_lab.registry));
    g_lab.analysis.enabledScopes = g_lab.view->stack().ids();

    // Last, so a previous visit lands on top of the defaults rather than
    // under them. The desktop restores in this same order.
    restorePreferencesNow();

    // AFTER the restore, because the stops quote the shortcut bindings and
    // those are preferences: built before it, the tour would have promised
    // whatever the defaults say and been wrong for anyone who had rebound a
    // key. Settling it is the last step, so a visitor who has not seen it
    // through finds it open.
    g_lab.tour = std::make_unique<GuidedTour>(labTourSteps(*g_lab.shortcuts));
    g_lab.tour->restoreSettled(g_lab.tourSettled);
    g_lab.panes->configureProjections();
}

/// Builds the picture's own texture, remade whenever its size changes.
void refreshDisplayTexture()
{
    if (g_lab.displayTexture == nullptr || g_lab.displayTexture->width() != g_lab.picture.display().width ||
        g_lab.displayTexture->height() != g_lab.picture.display().height) {
        g_lab.displayTexture =
            g_lab.graphics->createScopeTexture(g_lab.picture.display().width, g_lab.picture.display().height);
    }
    g_lab.displayTexture->upload(g_lab.picture.display());
}

}  // namespace
}  // namespace sidescopes

extern "C" {

/// The page writes RGBA straight into this buffer, so there is one copy
/// rather than two. Null if the size is not usable.
EMSCRIPTEN_KEEPALIVE uint8_t* labFrameBuffer(int width, int height)
{
    using namespace sidescopes;
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    return g_lab.picture.decodeInto(width, height);
}

/// Takes the picture the page just wrote. The global region belongs to the
/// virtual display and is initialized once by the first layout pass, so a
/// picture change never moves or reshapes it.
EMSCRIPTEN_KEEPALIVE void labFrameReady()
{
    using namespace sidescopes;
    // The decode is kept as it arrived and everything on screen is derived
    // from it. A new photograph inherits whatever the controls are set to,
    // which is what a visitor comparing two pictures under one adjustment
    // expects - and there is no state where the canvas and the scopes are
    // looking at different pixels.
    g_lab.picture.adoptDecoded();
    refreshDisplayTexture();
    g_lab.settingsDirty = true;
}

/// The seven controls, from the page. Values are the ImageAdjustments ranges:
/// exposure in stops, the rest from -1 to 1, all zero at rest.
///
/// The controls belong to the PAGE rather than to the application's window,
/// and deliberately: they stand for the editor a photographer has open beside
/// SideScopes. Drawn inside the application they would teach that SideScopes
/// edits photographs, which is the one thing this lab must not say.
EMSCRIPTEN_KEEPALIVE void labSetAdjustments(float exposure, float contrast, float highlights, float shadows,
                                            float temperature, float tint, float saturation)
{
    using namespace sidescopes;
    ImageAdjustments wanted;
    wanted.exposure = exposure;
    wanted.contrast = contrast;
    wanted.highlights = highlights;
    wanted.shadows = shadows;
    wanted.temperature = temperature;
    wanted.tint = tint;
    wanted.saturation = saturation;
    (void)g_lab.picture.setAdjustments(wanted);
}

/// Opens the walk-through from the first stop, however settled it is. The
/// page's "take the tour" button, and the only way back in once it has been
/// seen through or waved away.
EMSCRIPTEN_KEEPALIVE void labStartTour()
{
    using namespace sidescopes;
    g_lab.tour->start();
}

/// Where a control that belongs to the PAGE sits, in points relative to the
/// canvas's top-left. The document has to tell us, because the filmstrip and
/// the adjustment controls are part of it rather than of the application, and
/// they are the stops the walk-through names that this side cannot measure.
///
/// Controls outside the canvas may have negative coordinates. The page
/// highlights them itself, since the canvas cannot draw beyond its bounds.
EMSCRIPTEN_KEEPALIVE void labSetPageAnchor(const char* id, float left, float top, float right, float bottom)
{
    using namespace sidescopes;
    if (id == nullptr) {
        return;
    }
    g_lab.pageAnchors[id] = ImVec4{left, top, right, bottom};
}

/// Whether the walk-through is on the stop that names @p id, so the page can
/// light up its own control while it is.
EMSCRIPTEN_KEEPALIVE int labTourAtAnchor(const char* id)
{
    using namespace sidescopes;
    const TourStep* step = g_lab.tour->current();

    return step != nullptr && id != nullptr && step->anchor == id ? 1 : 0;
}

}  // extern "C"

int main()
{
    using namespace sidescopes;

    if (glfwInit() == GLFW_FALSE) {
        return 1;
    }
    g_lab.graphics = createGraphicsBackend();
    g_lab.graphics->setWindowHints();
    // The contrib GLFW port binds a window to a NAMED canvas, and that
    // binding is what its event listeners attach to. Without this the
    // context still renders - the default canvas backs it - and no pointer
    // or key event ever reaches the interface, which reads as a lab that
    // draws correctly and ignores the mouse.
    emscripten::glfw3::SetNextWindowCanvasSelector("#canvas");
    // Creating a focused GLFW window calls focus() on its canvas. Inside an
    // iframe that also focuses the frame in the parent and mobile browsers
    // scroll the website down to reveal it before the visitor has touched
    // anything. A standalone Lab can still take keyboard focus immediately;
    // an embedded one waits for the first deliberate interaction.
    // clang-format off
    const bool embedded = EM_ASM_INT({ return window.parent !== window; }) != 0;
    // clang-format on
    glfwWindowHint(GLFW_FOCUSED, embedded ? GLFW_FALSE : GLFW_TRUE);
    g_lab.window = glfwCreateWindow(560, 880, "SideScopes", nullptr, nullptr);
    if (g_lab.window == nullptr) {
        glfwTerminate();

        return 1;
    }
    // The canvas follows its container, so the shell can lay itself out side
    // by side on a wide page and stacked on a narrow one - the same choice a
    // desktop user makes by dragging the window.
    (void)emscripten::glfw3::MakeCanvasResizable(g_lab.window, "#stage");

    g_lab.monospaceFont = startImGui(g_lab.window);
    if (!g_lab.graphics->init(g_lab.window)) {
        return 1;
    }

    buildScopes();

    // The browser paces the loop; asking for a rate of our own would fight
    // requestAnimationFrame rather than pace anything.
    emscripten_set_main_loop(frame, 0, /*simulate_infinite_loop=*/1);

    return 0;
}
