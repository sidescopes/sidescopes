// The browser demo: the application's own scope panes, over a picture you
// can put a region on.
//
// This is not a port of SideScopes and cannot become one — no browser API
// reads another program's window, so the region tools have nothing to stand
// on. What it is, is the INSTRUMENT: the same engines, drawn by the same
// PaneArea the desktop build draws with, so the graticule, the target
// boxes, the skin-tone line, the dividers and the styling are the shipped
// ones rather than a second implementation that could drift from them.
//
// The one thing rebuilt rather than reused is the region border, because
// the desktop draws it as a native window on the desktop and a page has no
// desktop. It follows the same rules — see web/region_editor.h.
//
// The picture comes from the page: a bundled photograph, or a file the
// visitor chose. Nothing is uploaded, because there is no code here that
// could.

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#include "core/page_allocator.h"
#include "core/preferences.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/desktop.h"
#include "platform/graphics.h"
#include "platform/screen_capture.h"
#include "platform/web/screen_capture_source.h"
#include "web/demo_layout.h"
#include "web/demo_shell.h"
#include "web/demo_storage.h"
#include "web/demo_tour_steps.h"
#include "web/image_adjust.h"
#include "web/region_editor.h"

namespace sidescopes {
namespace {

constexpr float TitleBarHeight = 26.0f;

/// Everything the demo owns, in one place so the animation-frame callback
/// can reach it without a pile of globals.
struct Demo
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

    /// The picture as it was decoded, BGRA, never written to again. Every
    /// adjustment is computed FROM this, so dragging a control back and forth
    /// cannot degrade the photograph and leave the scopes reporting the
    /// damage as though it were the picture's own.
    std::vector<uint8_t> original;
    /// The picture the scopes read and the canvas shows: the same bytes, so
    /// the two can never disagree about what is being measured.
    std::vector<uint8_t> frame;
    int frameWidth = 0;
    int frameHeight = 0;
    bool frameDirty = false;
    /// New PIXELS, as against new settings. Kept apart so that dragging a
    /// region - which changes the settings every frame - does not also copy
    /// the whole picture into the capture source every frame.
    bool pictureDirty = false;

    /// The same picture RGBA, for showing it, and its texture.
    ScopeImage display;
    std::unique_ptr<ScopeTexture> displayTexture;

    /// The context menu's model wants both; neither does anything here -
    /// nothing can be attached in a page, and presets are not wired yet -
    /// but the menu is built from the real types rather than a stand-in.
    AttachController attach;
    LayoutPresetStore presets;
    /// The layout chip and its save button: the middle of the application's
    /// toolbar, and without it the row reads as two stray icons.
    std::unique_ptr<LayoutPresetController> presetController;
    std::unique_ptr<LayoutPresetPicker> presetPicker;
    /// The parameter entries the open menu was built with; menuScopeParam
    /// resolves a chosen id against them, so they outlive the build.
    std::vector<ParamMenuAction> menuParams;

    /// What the visitor has done to the picture, and whether it still needs
    /// applying. Applied once a frame rather than once an event, so dragging a
    /// slider costs one pass per redraw instead of one per pointer move.
    ImageAdjustments adjustments;
    bool adjustDirty = false;

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
    /// Where the picture last landed, so the pointer sample and the region
    /// gesture measure against the same rectangle.
    RegionEditor::Placement placement;
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

Demo g_demo;

/// Uploads g_demo.display to its texture; defined with the other graphics
/// work further down, needed by the adjustment pass just below.
void refreshDisplayTexture();

/// Runs the visitor's adjustments over the pristine decode, into the buffer
/// the scopes read AND the buffer the canvas draws.
///
/// One pass, one result, two consumers. If the picture on screen and the
/// pixels reaching the engines could ever differ, the demo would teach
/// something false about what a scope is measuring - and it would look
/// entirely correct while doing it.
void refreshAdjustedPicture()
{
    if (g_demo.original.empty()) {
        return;
    }
    const std::size_t pixels = g_demo.original.size() / 4u;
    g_demo.frame.resize(g_demo.original.size());
    applyAdjustments(g_demo.original.data(), g_demo.frame.data(), pixels, g_demo.adjustments);

    // The same bytes, in the order the canvas wants them.
    g_demo.display.rgba.resize(g_demo.frame.size());
    for (std::size_t at = 0; at + 3 < g_demo.frame.size(); at += 4) {
        g_demo.display.rgba[at] = g_demo.frame[at + 2];
        g_demo.display.rgba[at + 1] = g_demo.frame[at + 1];
        g_demo.display.rgba[at + 2] = g_demo.frame[at];
        g_demo.display.rgba[at + 3] = g_demo.frame[at + 3];
    }
    g_demo.display.sequence += 1u;
    refreshDisplayTexture();
    g_demo.pictureDirty = true;
}

/// The region the scopes read, as the worker wants it: a share of the frame
/// rather than a rectangle of pixels. Nothing at all when there is no region,
/// which is a STATE and not a failure - the worker then publishes nothing and
/// every scope is empty, exactly as the desktop is after Escape.
std::optional<RegionOfInterest> regionOfPicture()
{
    if (!g_demo.region.hasRegion() || g_demo.frameWidth <= 0 || g_demo.frameHeight <= 0) {
        return std::nullopt;
    }
    // platform/region_geometry's own conversion, which every platform's
    // overlay already uses. Writing the four divisions out here again looked
    // harmless and is exactly how the grab zones drifted.
    const SsRect rect = g_demo.region.rect();
    const LocalRect local{static_cast<double>(rect.x), static_cast<double>(rect.y), static_cast<double>(rect.width),
                          static_cast<double>(rect.height)};

    return regionFromLocalRect(local, g_demo.frameWidth, g_demo.frameHeight);
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
    if (g_demo.adjustDirty) {
        refreshAdjustedPicture();
        g_demo.adjustDirty = false;
    }
    if (g_demo.pictureDirty && !g_demo.frame.empty()) {
        submitCapturedPicture(g_demo.frame.data(), g_demo.frameWidth, g_demo.frameHeight);
        g_demo.pictureDirty = false;
    }
    if (g_demo.frameDirty) {
        // The region the editor holds is in PICTURE pixels; the worker reads
        // percentages of the frame, so a selection survives the frame being
        // a different size - which is the same reason the desktop stores it
        // that way.
        g_demo.analysis.region = regionOfPicture();
        g_demo.worker->updateSettings(g_demo.analysis);
        g_demo.frameDirty = false;
    }
    // The worker has no thread of its own here, so its pass runs on this one.
    g_demo.worker->pump();
    (void)g_demo.worker->fetchOutput(g_demo.outputVersion, g_demo.output);
}

/// Fits the picture into @p area, centred, never enlarged past its own
/// pixels — a photograph blown up would invite reading detail that is not
/// there.
[[nodiscard]] RegionEditor::Placement placePicture(const ImVec2& cursor, const ImVec2& area)
{
    const float wide = area.x / static_cast<float>(std::max(1, g_demo.display.width));
    const float tall = area.y / static_cast<float>(std::max(1, g_demo.display.height));
    const float scale = std::min({wide, tall, 1.0f});
    const float width = static_cast<float>(g_demo.display.width) * scale;
    const float height = static_cast<float>(g_demo.display.height) * scale;

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
    if (!g_demo.pinArmed) {
        return false;
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
        g_demo.pinning = true;
        g_demo.pinFrom = mouse;
    }
    if (!g_demo.pinning) {
        return true;
    }
    const float scale = placement.scale > 0.0f ? placement.scale : 1.0f;
    const ImVec2 from{std::min(g_demo.pinFrom.x, mouse.x), std::min(g_demo.pinFrom.y, mouse.y)};
    const ImVec2 to{std::max(g_demo.pinFrom.x, mouse.x), std::max(g_demo.pinFrom.y, mouse.y)};
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (to.x - from.x > 2.0f || to.y - from.y > 2.0f)) {
        ImGui::GetWindowDrawList()->AddRect(from, to, IM_COL32(255, 255, 255, 200));
    }
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        return true;
    }
    g_demo.pinning = false;
    const SsRect area{static_cast<int>((from.x - placement.origin.x) / scale),
                      static_cast<int>((from.y - placement.origin.y) / scale),
                      std::max(1, static_cast<int>((to.x - from.x) / scale)),
                      std::max(1, static_cast<int>((to.y - from.y) / scale))};
    const std::optional<FloatColor> colour =
        shell::averageOver(area, g_demo.display.rgba, g_demo.display.width, g_demo.display.height);
    if (colour.has_value()) {
        g_demo.pins->pin(*colour);
        g_demo.saveDue = true;
    }
    // Shift keeps the tool up to pin several, as the application does.
    g_demo.pinArmed = shell::modifiers().shift;
    g_demo.panes->setStatus(g_demo.pinArmed ? "Pinned - keep pinning, or release Shift"
                                            : (colour.has_value() ? "Colour pinned" : "Nothing under the pointer"));

    return true;
}

/// The picture, with the region on it. @return Whether the region moved.
[[nodiscard]] bool drawPicture(const ImVec2& position, const ImVec2& area)
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
    if (g_demo.displayTexture != nullptr && g_demo.display.width > 0) {
        g_demo.placement = placePicture(ImGui::GetCursorScreenPos(), area);
        const ImVec2 size{static_cast<float>(g_demo.display.width) * g_demo.placement.scale,
                          static_cast<float>(g_demo.display.height) * g_demo.placement.scale};
        ImGui::GetWindowDrawList()->AddImage(
            g_demo.displayTexture->textureId(), g_demo.placement.origin,
            ImVec2{g_demo.placement.origin.x + size.x, g_demo.placement.origin.y + size.y});
        if (!runPinTool(g_demo.placement)) {
            moved = g_demo.region.update(g_demo.placement, g_demo.display.width, g_demo.display.height);
        }
        // The desktop samples the screen under the pointer; the only picture
        // here is this one, so it is sampled the same way.
        const std::optional<FloatColor> live = shell::sampleAt(
            ImGui::GetMousePos(), g_demo.placement, g_demo.display.rgba, g_demo.display.width, g_demo.display.height);
        if (live.has_value()) {
            g_demo.readoutColour = live;
        }
        // The trace markers mean "this colour, in the region being measured",
        // so they go quiet the moment the pointer leaves it - exactly as they
        // do on the desktop.
        const SsRect region = g_demo.region.rect();
        const float scale = g_demo.placement.scale > 0.0f ? g_demo.placement.scale : 1.0f;
        const ImVec2 mouse = ImGui::GetMousePos();
        const int imageX = static_cast<int>((mouse.x - g_demo.placement.origin.x) / scale);
        const int imageY = static_cast<int>((mouse.y - g_demo.placement.origin.y) / scale);
        const bool inRegion = g_demo.region.hasRegion() && imageX >= region.x && imageY >= region.y &&
                              imageX < region.x + region.width && imageY < region.y + region.height;
        g_demo.traceColour = inRegion ? live : std::nullopt;
    } else {
        ImGui::TextDisabled("Choose a picture to measure.");
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    return moved;
}

/// What a click on a chip or a tool decided, carried out. The host does this
/// on the desktop too; only the region actions differ, because a region here
/// is a rectangle on a picture rather than on a desktop.
void applyOutcome(const PaneRenderOutcome& outcome)
{
    if (outcome.chosenScope.has_value()) {
        const ScopeChoice& choice = *outcome.chosenScope;
        (void)g_demo.view->stack().choose(choice.id, choice.stack);
        g_demo.analysis.enabledScopes = g_demo.view->stack().ids();
        g_demo.frameDirty = true;
    }
    if (outcome.clearRegion) {
        // The desktop's own answer: the region goes, and the scopes read
        // nothing. An empty scope is a state rather than a failure, and a
        // demo that quietly selected the whole picture instead would be
        // teaching something the application does not do.
        g_demo.region.clear();
        g_demo.panes->releaseTraces();
        g_demo.frameDirty = true;
    }
    if (outcome.analysisDirty) {
        g_demo.panes->configureProjections();
        g_demo.frameDirty = true;
    }
    if (outcome.chosenScope.has_value() || outcome.analysisDirty || outcome.preferencesSaveDue) {
        g_demo.saveDue = true;
    }
}

/// One shortcut action, carried out. Split from the scan so neither grows
/// past what one screen holds.
/// A preset outcome, applied. The controller does the work; the host pushes
/// what changed, exactly as the desktop one does.
void applyPreset(const LayoutPresetOutcome& outcome)
{
    if (!outcome.status.empty()) {
        g_demo.panes->setStatus(outcome.status);
    }
    if (outcome.analysisDirty) {
        g_demo.analysis.enabledScopes = g_demo.view->stack().ids();
        g_demo.panes->configureProjections();
        g_demo.frameDirty = true;
    }
    g_demo.saveDue = true;
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
        g_demo.view->setZoom(action.zoomLevel);
        g_demo.frameDirty = true;
        break;
    case ShortcutAction::Kind::ClearRegion:
        outcome.clearRegion = true;
        applyOutcome(outcome);
        break;
    case ShortcutAction::Kind::RequestPick:
        // Through the picker, not straight to the tool: the toolbar
        // buttons raise their requests the same way, so the keyboard and
        // the buttons meet at one place instead of two.
        g_demo.picker->request(action.pickMode);
        break;
    case ShortcutAction::Kind::LoadPreset:
        applyPreset(g_demo.presetController->load(action.presetSlot));
        break;
    case ShortcutAction::Kind::CopyPresetTo:
    case ShortcutAction::Kind::SaveActivePreset:
        applyPreset(g_demo.presetController->saveInto(action.kind == ShortcutAction::Kind::CopyPresetTo
                                                          ? action.presetSlot
                                                          : g_demo.presetController->activeSlot()));
        break;
    // NO `default:`, deliberately, and this is the whole guard. Twice a
    // catch-all here silently swallowed an action the resolver was emitting -
    // D did nothing, then the preset digits did nothing - and neither
    // failed, they just quietly did not happen. Listing every kind makes the
    // NEXT one a compile error under -Werror instead, which is how App's own
    // switch has always been written and why App never lost one.
    //
    // The window chords belong to the browser: a page that intercepted them
    // would be taking the reader's own window controls away, and the platform
    // seams already answer false for all three so the resolver never emits
    // them here.
    case ShortcutAction::Kind::HideApplication:
    case ShortcutAction::Kind::MinimizeWindow:
    case ShortcutAction::Kind::QuitWindow:
        break;
    // The settings window is not built in the demo, so there is nothing to
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
        shortcutContextFor(*g_demo.view, *g_demo.registry, /*settingsOpen=*/false, /*wantsTextInput=*/false);
    for (const ShortcutAction& action :
         g_demo.shortcuts->resolvePressed(context, shell::modifiers(), shell::keyPressed)) {
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
            *g_demo.view,  *g_demo.registry,       *g_demo.shortcuts,       g_demo.analysis.scopeParams,
            g_demo.attach, g_demo.presets.all(),   g_demo.pins->empty(),    0,
            1.0f,          QualityLevel::Standard, /*regionSelected=*/true,
        };
        std::vector<NativeMenuItem> items;
        g_demo.menuParams.clear();
        buildContextMenu(model, clickedPane, items, g_demo.menuParams);
        openImGuiContextMenu(std::move(items));
    }

    const ImGuiContextMenuFrame menu = drawImGuiContextMenu();
    if (menu.chosen < 0) {
        return;
    }
    if (const std::optional<std::string> scope = menuScopeToggle(menu.chosen, *g_demo.registry)) {
        PaneRenderOutcome outcome;
        outcome.chosenScope = ScopeChoice{*scope, /*stack=*/true};
        applyOutcome(outcome);
    } else if (const ParamMenuAction* param = menuScopeParam(menu.chosen, g_demo.menuParams)) {
        g_demo.analysis.scopeParams[param->scopeId][param->paramKey] = param->value;
        g_demo.panes->configureProjections();
        g_demo.frameDirty = true;
    } else if (const std::optional<float> strength = menuGraticuleStrength(menu.chosen)) {
        g_demo.view->setGraticuleStrength(*strength);
    } else if (const std::optional<ShortcutAction> action = menuShortcutAction(menu.chosen)) {
        applyShortcut(*action);
    }
    // Everything else is left alone on purpose. The layout orientation lives
    // with the pane renderer rather than the view and has no setter reachable
    // from here; the presets, the settings window, the diagnostics and the
    // window chords are desktop actions a page does not carry out.
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
    glfwGetFramebufferSize(g_demo.window, &width, &height);
    if (width <= 0 || height <= 0 || !g_demo.graphics->beginFrame(width, height)) {
        return;
    }

    ImGui::NewFrame();
    drawShell();
    ImGui::Render();
    g_demo.graphics->endFrame();
}

/// The window's own chrome: a title bar, an edge, and a shadow under it.
/// The desktop gets all three from the operating system; a page gets none
/// of them, and without them the application reads as a frame around the
/// picture rather than a window floating over it.
void drawWindowChrome(const ImVec2& position, const ImVec2& size)
{
    ImDrawList* under = ImGui::GetBackgroundDrawList();
    // A soft shadow, built from a few expanding rounded rectangles - enough
    // to lift the window off what is behind it.
    for (int step = 6; step >= 1; --step) {
        const float spread = static_cast<float>(step) * 1.6f;
        under->AddRectFilled(ImVec2{position.x - spread, position.y - spread + 2.0f},
                             ImVec2{position.x + size.x + spread, position.y + size.y + spread + 2.0f},
                             IM_COL32(0, 0, 0, 16), 10.0f + spread);
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 corner{position.x + size.x, position.y + size.y};
    draw->AddRectFilled(position, ImVec2{corner.x, position.y + TitleBarHeight}, IM_COL32(38, 36, 35, 255), 10.0f,
                        ImDrawFlags_RoundCornersTop);
    const char* name = "SideScopes";
    const ImVec2 text = ImGui::CalcTextSize(name);
    draw->AddText(ImVec2{position.x + (size.x - text.x) * 0.5f, position.y + (TitleBarHeight - text.y) * 0.5f},
                  IM_COL32(196, 190, 186, 255), name);
    // One hairline around the whole thing: the edge the operating system
    // would have drawn.
    draw->AddRect(position, corner, IM_COL32(74, 70, 67, 255), 10.0f, 0, 1.0f);
}

/// The application's window: the toolbar, the panes and the status strip,
/// drawn as its own window rather than as a band of the page - because on a
/// desktop that is exactly what it is.
void drawAppWindow(const ShellLayout& layout, const PaneRenderInput& input)
{
    ImGui::SetNextWindowPos(ImVec2{layout.appPos.x, layout.appPos.y});
    ImGui::SetNextWindowSize(ImVec2{layout.appSize.x, layout.appSize.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##app", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    drawWindowChrome(ImVec2{layout.appPos.x, layout.appPos.y}, ImVec2{layout.appSize.x, layout.appSize.y});
    ImGui::Dummy(ImVec2{0.0f, TitleBarHeight});

    applyShortcuts();
    // Each toolbar group is wrapped so the walk-through can point at it: a
    // group's bounds are what GetItemRect reports once it ends, which is what
    // groups are for, and it means the tour follows the controls rather than
    // coordinates written down once and left to rot.
    ImGui::BeginGroup();
    applyOutcome(g_demo.panes->drawScopeToggles(shell::modifiers().shift));
    ImGui::EndGroup();
    g_demo.anchors.note("chooser", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    // The preset chip sits between the scope selector and the region tools,
    // as it does on the desktop.
    (void)g_demo.presetPicker->draw(g_demo.panes->icons());
    ImGui::SameLine();
    applyOutcome(g_demo.panes->drawRegionToolIcons(input));

    ImGui::BeginGroup();
    applyOutcome(g_demo.panes->drawScopePanes(input));
    ImGui::EndGroup();
    g_demo.anchors.note("scopes", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    g_demo.panes->drawStatusBar(input);
    // Asked of the controls themselves. The region tools are right-aligned
    // and the pin sits at the other end of the application, so a group around
    // either call would frame most of the window instead of two buttons.
    noteControlAnchor("tools", g_demo.panes->regionToolBounds());
    noteControlAnchor("pin", g_demo.panes->pinToolBounds());
    for (const auto& [id, bounds] : g_demo.pageAnchors) {
        g_demo.anchors.note(id, ImVec2{bounds.x, bounds.y}, ImVec2{bounds.z, bounds.w});
    }
    // Only over the application. Right-clicking the picture is the
    // workspace's business on a desktop - there it would raise the editor's
    // menu, not this one - so it raises nothing here.
    drawContextMenu(g_demo.panes->paneAt(ImGui::GetMousePos()), ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));

    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// A region tool was asked for. The desktop opens a picker over the whole
/// desktop; there is none here, so the draw tool arms the gesture on the
/// picture and the two that need a desktop say plainly that they cannot.
void answerPickRequest()
{
    const std::optional<RegionPickerMode> want = g_demo.picker->pendingRequest();
    if (!want.has_value()) {
        return;
    }
    g_demo.picker->clearRequest();
    if (*want == RegionPickerMode::PinColor) {
        // Pinning needs no desktop: it samples the picture, which is right
        // here. Refusing it was simply wrong.
        g_demo.pinArmed = true;
        g_demo.panes->setStatus("Click a colour to pin it, or drag to pin an area's average");

        return;
    }
    if (*want == RegionPickerMode::DrawGlobal) {
        g_demo.region.armDraw();
        g_demo.panes->setStatus("Drag on the picture to draw a region");

        return;
    }
    g_demo.panes->setStatus("Attaching to a window or a face needs the desktop application");
}

/// Notes a control's own rectangle, when it has drawn at least once.
void noteControlAnchor(const char* id, const std::optional<ImVec4>& bounds)
{
    if (!bounds) {
        return;
    }
    g_demo.anchors.note(id, ImVec2{bounds->x, bounds->y}, ImVec2{bounds->z, bounds->w});
}

/// Where the picture and the region landed, for the walk-through to point at.
/// These are the two the demo owns; the toolbar and the panes note their own
/// as they draw, which is what keeps the tour pointing at controls rather
/// than at coordinates written down once and left to rot.
void notePictureAnchors()
{
    const RegionEditor::Placement& at = g_demo.placement;
    g_demo.anchors.note("picture", at.origin,
                        ImVec2{at.origin.x + static_cast<float>(g_demo.display.width) * at.scale,
                               at.origin.y + static_cast<float>(g_demo.display.height) * at.scale});
    if (!g_demo.region.hasRegion()) {
        return;
    }
    const SsRect rect = g_demo.region.rect();
    const ImVec2 topLeft{at.origin.x + static_cast<float>(rect.x) * at.scale,
                         at.origin.y + static_cast<float>(rect.y) * at.scale};
    g_demo.anchors.note("region", topLeft,
                        ImVec2{topLeft.x + static_cast<float>(rect.width) * at.scale,
                               topLeft.y + static_cast<float>(rect.height) * at.scale});
}

/// The walk-through, drawn last so its veil covers what it is talking about,
/// with its outcome applied in one place as every other outcome here is.
void runTour(const ImVec2& shellPos, const ImVec2& shellSize)
{
    const ImVec2 shellMax{shellPos.x + shellSize.x, shellPos.y + shellSize.y};
    switch (drawTourOverlay(*g_demo.tour, g_demo.anchors, shellPos, shellMax)) {
    case TourAction::Advance:
        g_demo.tour->advance();
        // Settling is worth remembering; which step was reached is not.
        g_demo.saveDue = g_demo.saveDue || g_demo.tour->settled();
        break;
    case TourAction::Skip:
        g_demo.tour->skip();
        g_demo.saveDue = true;
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
        /*regionSelected=*/g_demo.region.hasRegion(),
        /*pinsAvailable=*/true,
        /*vectorscopeColor=*/g_demo.traceColour,
        /*waveformColor=*/g_demo.traceColour,
        /*readoutColor=*/g_demo.readoutColour,
        /*monospaceFont=*/g_demo.monospaceFont,
    };

    // Dropped every frame, so a control that stops drawing stops being
    // pointed at rather than leaving the tour aimed at where it used to be.
    g_demo.anchors.clear();

    if (drawPicture(ImVec2{layout.screenPos.x, layout.screenPos.y}, ImVec2{layout.screenSize.x, layout.screenSize.y})) {
        g_demo.frameDirty = true;
    }
    notePictureAnchors();
    // The border's own close badge dismisses the region, and it means the
    // same thing the toolbar's clear does - so it goes through the same path.
    if (g_demo.region.takeDismissed()) {
        PaneRenderOutcome dismissed;
        dismissed.clearRegion = true;
        applyOutcome(dismissed);
    }
    analyse();
    g_demo.panes->uploadVisibleScopes(g_demo.region.hasRegion());

    drawAppWindow(layout, input);
    answerPickRequest();

    // The region tools want a crosshair, which Dear ImGui's cursor enum does
    // not carry, so the canvas is told directly. Handing it back to Dear
    // ImGui the moment neither tool is up keeps the resize cursors working.
    if (g_demo.pinArmed) {
        // Crosshair AND swatch in the cursor image, so neither can trail the
        // pointer - the desktop's construction, for the same reason.
        shell::setPinCursor(g_demo.readoutColour);
    } else if (g_demo.region.armed()) {
        shell::setCanvasCursor("crosshair");
    } else {
        shell::setCanvasCursor(nullptr);
    }

    runTour(viewport->WorkPos, viewport->WorkSize);

    if (g_demo.saveDue) {
        savePreferencesNow();
        g_demo.saveDue = false;
    }
}

/// Reads the running session back out and keeps it for next time. The
/// application's own capture does the translating; the shell adds what it
/// alone holds, which here is the preset slots.
void savePreferencesNow()
{
    Preferences saved = capturePreferences(*g_demo.view, *g_demo.pins, *g_demo.shortcuts, g_demo.analysis);
    saved.layoutPresets = g_demo.presetController->all();
    saved.layoutActiveSlot = g_demo.presetController->activeSlot();
    saved.tourSettled = g_demo.tour->settled() ? 1 : 0;
    if (!savePreferences(saved, preferencesFilePath())) {
        return;
    }
    const sidescopes::MappedFile file = sidescopes::mapFileReadOnly(preferencesFilePath().c_str());
    if (!file.valid()) {
        return;
    }
    storage::writeSaved(std::string(reinterpret_cast<const char*>(file.data), file.size));
}

/// Puts a previous visit back, if this browser holds one. Whatever is
/// missing or malformed simply defaults - the application already tolerates
/// an old or hand-edited file, and a demo that refused to start over a
/// preference would be worse than one that forgets.
void restorePreferencesNow()
{
    const std::string text = storage::readSaved();
    if (text.empty()) {
        // Nothing remembered, so this is a first visit - which is precisely
        // who the walk-through is for. Recorded explicitly rather than left
        // to the read below, because that path is not taken at all here and a
        // tour that opened for everyone EXCEPT a newcomer would be the exact
        // opposite of the point.
        g_demo.tourSettled = false;

        return;
    }
    std::filesystem::create_directories(std::filesystem::path(preferencesFilePath()).parent_path());
    {
        std::ofstream out(preferencesFilePath(), std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    const Preferences saved = loadPreferences(preferencesFilePath());
    restorePreferences(saved, *g_demo.view, *g_demo.pins, *g_demo.shortcuts, g_demo.analysis);
    g_demo.presetController->restore(saved.layoutPresets, saved.layoutActiveSlot);
    // Applied by the caller once the tour exists: it is built after this, so
    // its text can quote the bindings this just restored.
    g_demo.tourSettled = saved.tourSettled != 0;
    g_demo.analysis.enabledScopes = g_demo.view->stack().ids();
}

/// Everything downstream of the window: the registry, the view, the capture
/// seams and the pane renderer. Split out of main so that neither runs past
/// what one screen holds.

void buildScopes()
{
    // builtinModules() is the registry the desktop build uses too: the
    // module entries are linked in and register themselves.
    g_demo.registry = std::make_unique<ScopeRegistry>(builtinModules());
    g_demo.view = std::make_unique<ScopeView>(*g_demo.registry);
    g_demo.pins = std::make_unique<PinBoard>();
    g_demo.capture = createScreenCaptureSource();
    g_demo.captureController = std::make_unique<CaptureController>(*g_demo.capture, g_demo.mailbox);
    (void)g_demo.captureController->requestPermission();
    // A paused pipeline is not a dead one — the controller's own words. The
    // demo asks for no stream because there is no screen to stream, which is
    // precisely what suspend() describes; without it dead() is true and the
    // panes draw a "capture was interrupted" page over the scopes.
    // The picture arrives through the capture source like any other frame, so
    // the stream is genuinely running - the target is the page rather than a
    // display, and the host feeds it.
    (void)g_demo.captureController->start();

    seedImageSizes(g_demo.analysis);

    // The REAL worker, running its passes on this thread. A page has no
    // threads to give, so it is pumped from the frame loop rather than
    // started on its own; the passes are the same passes the desktop runs.
    g_demo.worker = std::make_unique<AnalysisWorker>(g_demo.mailbox);
    g_demo.worker->startInline();
    g_demo.picker = std::make_unique<RegionPicker>(*g_demo.captureController, *g_demo.worker, *g_demo.capture);
    g_demo.shortcuts = std::make_unique<ShortcutResolver>(*g_demo.registry);
    g_demo.presetController = std::make_unique<LayoutPresetController>(*g_demo.view, *g_demo.registry, g_demo.analysis);
    g_demo.presetPicker = std::make_unique<LayoutPresetPicker>(*g_demo.presetController);

    const ScopePaneContext context{
        *g_demo.graphics,          *g_demo.view,   *g_demo.registry, g_demo.analysis,   g_demo.output,
        *g_demo.captureController, *g_demo.picker, *g_demo.pins,     *g_demo.shortcuts,
    };
    g_demo.panes = std::make_unique<ScopePaneRenderer>(context, createProjectionInstances(*g_demo.registry),
                                                       createScopeTextures(*g_demo.registry));
    g_demo.analysis.enabledScopes = g_demo.view->stack().ids();

    // Last, so a previous visit lands on top of the defaults rather than
    // under them. The desktop restores in this same order.
    restorePreferencesNow();

    // AFTER the restore, because the stops quote the shortcut bindings and
    // those are preferences: built before it, the tour would have promised
    // whatever the defaults say and been wrong for anyone who had rebound a
    // key. Settling it is the last step, so a visitor who has not seen it
    // through finds it open.
    g_demo.tour = std::make_unique<GuidedTour>(demoTourSteps(*g_demo.shortcuts));
    g_demo.tour->restoreSettled(g_demo.tourSettled);
    g_demo.panes->configureProjections();
}

/// Builds the picture's own texture, remade whenever its size changes.
void refreshDisplayTexture()
{
    if (g_demo.displayTexture == nullptr || g_demo.displayTexture->width() != g_demo.display.width ||
        g_demo.displayTexture->height() != g_demo.display.height) {
        g_demo.displayTexture = g_demo.graphics->createScopeTexture(g_demo.display.width, g_demo.display.height);
    }
    g_demo.displayTexture->upload(g_demo.display);
}

}  // namespace
}  // namespace sidescopes

extern "C" {

/// The page writes RGBA straight into this buffer, so there is one copy
/// rather than two. Null if the size is not usable.
EMSCRIPTEN_KEEPALIVE uint8_t* demoFrameBuffer(int width, int height)
{
    using namespace sidescopes;
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    g_demo.frameWidth = width;
    g_demo.frameHeight = height;
    g_demo.frame.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0u);

    return g_demo.frame.data();
}

/// Takes the picture the page just wrote: kept RGBA for showing, swizzled to
/// BGRA for the engines, and given a region to start from.
EMSCRIPTEN_KEEPALIVE void demoFrameReady()
{
    using namespace sidescopes;
    g_demo.display.width = g_demo.frameWidth;
    g_demo.display.height = g_demo.frameHeight;
    g_demo.display.sequence += 1u;
    g_demo.display.rgba = g_demo.frame;
    refreshDisplayTexture();

    for (std::size_t at = 0; at + 3 < g_demo.frame.size(); at += 4) {
        const uint8_t red = g_demo.frame[at];
        g_demo.frame[at] = g_demo.frame[at + 2];
        g_demo.frame[at + 2] = red;
    }
    // The decode is kept as it arrived, and the picture on screen is derived
    // from it. A new photograph inherits whatever the controls are set to,
    // which is what a visitor comparing two pictures under one adjustment
    // expects - and it means there is no state where the canvas and the
    // scopes are looking at different pixels.
    g_demo.original = g_demo.frame;
    g_demo.adjustDirty = true;
    // The region STAYS WHERE IT IS. It is a rectangle on what stands in for a
    // display, and changing the photograph beneath it is no more reason to
    // move it than changing the photograph in an editor is on a desktop.
    // Centring a fresh one took its proportions from each new picture, so a
    // landscape region became a portrait one because the photograph did.
    //
    // The first picture has none to hold, and gets one to start from.
    if (g_demo.region.hasRegion()) {
        g_demo.region.holdOnScreen(g_demo.placement);
    } else {
        g_demo.region.reset(g_demo.frameWidth, g_demo.frameHeight);
    }
    g_demo.pictureDirty = true;
    g_demo.frameDirty = true;
}

/// The seven controls, from the page. Values are the ImageAdjustments ranges:
/// exposure in stops, the rest from -1 to 1, all zero at rest.
///
/// The controls belong to the PAGE rather than to the application's window,
/// and deliberately: they stand for the editor a photographer has open beside
/// SideScopes. Drawn inside the application they would teach that SideScopes
/// edits photographs, which is the one thing this demo must not say.
EMSCRIPTEN_KEEPALIVE void demoSetAdjustments(float exposure, float contrast, float highlights, float shadows,
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
    if (wanted == g_demo.adjustments) {
        return;
    }
    g_demo.adjustments = wanted;
    g_demo.adjustDirty = true;
}

/// Opens the walk-through from the first stop, however settled it is. The
/// page's "take the tour" button, and the only way back in once it has been
/// seen through or waved away.
EMSCRIPTEN_KEEPALIVE void demoStartTour()
{
    using namespace sidescopes;
    g_demo.tour->start();
}

/// Where a control that belongs to the PAGE sits, in points relative to the
/// canvas's top-left. The document has to tell us, because the filmstrip and
/// the adjustment controls are part of it rather than of the application, and
/// they are the stops the walk-through names that this side cannot measure.
///
/// Their y is NEGATIVE - both sit above the canvas - which is exactly what
/// puts the bubble at the top of the application, pointing the right way. The
/// page highlights the control itself, since nothing drawn here can reach it.
EMSCRIPTEN_KEEPALIVE void demoSetPageAnchor(const char* id, float left, float top, float right, float bottom)
{
    using namespace sidescopes;
    if (id == nullptr) {
        return;
    }
    g_demo.pageAnchors[id] = ImVec4{left, top, right, bottom};
}

/// Whether the walk-through is on the stop that names @p id, so the page can
/// light up its own control while it is.
EMSCRIPTEN_KEEPALIVE int demoTourAtAnchor(const char* id)
{
    using namespace sidescopes;
    const TourStep* step = g_demo.tour->current();

    return step != nullptr && id != nullptr && step->anchor == id ? 1 : 0;
}

}  // extern "C"

int main()
{
    using namespace sidescopes;

    if (glfwInit() == GLFW_FALSE) {
        return 1;
    }
    g_demo.graphics = createGraphicsBackend();
    g_demo.graphics->setWindowHints();
    // The contrib GLFW port binds a window to a NAMED canvas, and that
    // binding is what its event listeners attach to. Without this the
    // context still renders - the default canvas backs it - and no pointer
    // or key event ever reaches the interface, which reads as a demo that
    // draws correctly and ignores the mouse.
    emscripten::glfw3::SetNextWindowCanvasSelector("#canvas");
    g_demo.window = glfwCreateWindow(560, 880, "SideScopes", nullptr, nullptr);
    if (g_demo.window == nullptr) {
        glfwTerminate();

        return 1;
    }
    // The canvas follows its container, so the shell can lay itself out side
    // by side on a wide page and stacked on a narrow one - the same choice a
    // desktop user makes by dragging the window.
    (void)emscripten::glfw3::MakeCanvasResizable(g_demo.window, "#stage");

    g_demo.monospaceFont = startImGui(g_demo.window);
    if (!g_demo.graphics->init(g_demo.window)) {
        return 1;
    }

    buildScopes();

    // The browser paces the loop; asking for a rate of our own would fight
    // requestAnimationFrame rather than pace anything.
    emscripten_set_main_loop(frame, 0, /*simulate_infinite_loop=*/1);

    return 0;
}
