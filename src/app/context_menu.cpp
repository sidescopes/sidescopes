#include "app/context_menu.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/scope_layout.h"
#include "app/ui_scaling.h"
#include "core/diagnostics.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// Three id ranges run consecutively from their bases, one entry per offered
// step, and the last of them is open-ended: a scope toggle exists for every
// registered scope. Adding a step to either of the first two silently walked
// into the next range, so the build checks the ranges instead.
static_assert(MenuUiScaleBase + static_cast<int>(UiScaleSteps.size()) <= MenuQualityBase,
              "the interface-size ids run into the quality ids");
static_assert(MenuQualityBase + static_cast<int>(QualityLevels.size()) <= MenuShowScopeBase,
              "the quality ids run into the scope-toggle ids");

void menuAction(std::vector<NativeMenuItem>& menu, const char* label, int id, bool checked, std::string shortcut = "")
{
    menu.push_back({NativeMenuItem::Kind::Action, label, id, checked, std::move(shortcut)});
}

void menuSeparator(std::vector<NativeMenuItem>& menu)
{
    menu.push_back({NativeMenuItem::Kind::Separator, "", -1, false, ""});
}

void menuSubmenu(std::vector<NativeMenuItem>& menu, const char* label)
{
    menu.push_back({NativeMenuItem::Kind::SubmenuBegin, label, -1, false, ""});
}

void menuEndSubmenu(std::vector<NativeMenuItem>& menu)
{
    menu.push_back({NativeMenuItem::Kind::SubmenuEnd, "", -1, false, ""});
}

const std::map<std::string, double>& paramsFor(const ContextMenuModel& model, std::string_view id)
{
    static const std::map<std::string, double> noParams;
    const auto stored = model.scopeParams.find(std::string{id});

    return stored != model.scopeParams.end() ? stored->second : noParams;
}

bool scopeHasOptions(const ScopeRegistry& registry, std::string_view id)
{
    if (id == VectorscopeScopeId || id == ColorPickerScopeId) {
        return true;  // host sections: zoom and pins
    }
    const HostScope* hostScope = registry.byId(id);

    return hostScope != nullptr && hostScope->descriptor != nullptr &&
           firstParamOfKind(hostScope->descriptor, SS_PARAM_CHOICE) != nullptr;
}

void appendPinOptions(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    // Pins are a scope tool: they mark the vectorscope and the color picker, so
    // their submenu rides those scopes' own sections.
    menuSubmenu(menu, "Pins");
    menuAction(menu, "Pin Colors...", MenuPinColor, false, shortcutLabel(model.shortcuts.bindings().pinColor));
    if (!model.pinsEmpty) {
        menuAction(menu, "Clear Pinned Markers", MenuClearPinnedMarkers, false);
    }
    menuEndSubmenu(menu);
}

void appendZoomOptions(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    // The vectorscope's magnify viewport is a host control, not a module
    // parameter, so it stays hand-built beside the descriptor choices.
    menuSubmenu(menu, "Zoom");
    menuAction(menu, "1x", MenuZoom1, model.view.zoom() == 1,
               shortcutLabel(model.shortcuts.bindings().vectorscopeZoom));
    menuAction(menu, "2x", MenuZoom2, model.view.zoom() == 2,
               shortcutLabel(model.shortcuts.bindings().vectorscopeZoom));
    menuAction(menu, "4x", MenuZoom4, model.view.zoom() == 4,
               shortcutLabel(model.shortcuts.bindings().vectorscopeZoom));
    menuEndSubmenu(menu);
}

void appendScopeOptions(const ContextMenuModel& model, std::string_view id, bool flatten,
                        std::vector<NativeMenuItem>& menu, std::vector<ParamMenuAction>& paramActions)
{
    // A scope's own options: its descriptor's choice submenus, then any host
    // sections it carries. `flatten` lets a lone choice sit directly under an
    // enclosing scope-name submenu.
    const HostScope* hostScope = model.registry.byId(id);
    if (hostScope != nullptr && hostScope->descriptor != nullptr) {
        appendScopeChoiceMenus(*hostScope->descriptor, paramsFor(model, id), flatten, menu, paramActions);
    }
    if (id == VectorscopeScopeId) {
        appendZoomOptions(model, menu);
        appendPinOptions(model, menu);
    } else if (id == ColorPickerScopeId) {
        appendPinOptions(model, menu);
    }
}

void appendScopesSubmenu(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    menuSubmenu(menu, "Scopes");
    // Every registered scope gets an entry, in the order the user keeps them
    // in, so this list and the toolbar's selector read the same. The id stays
    // the scope's REGISTRY index, which nothing the user does can change, so a
    // new module appears here with no edit. A module scope names itself; the
    // one host scope (the colour picker, which carries no descriptor) is the
    // single exception.
    for (const std::string& id : model.view.order().ids()) {
        const HostScope* scope = model.registry.byId(id);
        if (scope == nullptr) {
            continue;
        }
        const char* name = scope->descriptor != nullptr ? scope->descriptor->name : "Color Picker";
        menuAction(menu, name, MenuShowScopeBase + model.registry.indexOf(id), model.view.stack().shows(id),
                   shortcutLabel(model.shortcuts.bindingFor(id)));
    }
    menuEndSubmenu(menu);
}

void appendPerScopeOptions(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu,
                           std::vector<ParamMenuAction>& paramActions)
{
    // On a background or toolbar click, each visible scope's options ride under
    // its own name, in toolbar order.
    for (const HostScope& scope : model.registry.scopes()) {
        if (!model.view.stack().shows(scope.id)) {
            continue;
        }
        // The vectorscope's section already carries the pins; the color picker
        // shows them only when the vectorscope is gone.
        if (scope.id == ColorPickerScopeId && model.view.stack().shows(VectorscopeScopeId)) {
            continue;
        }
        if (!scopeHasOptions(model.registry, scope.id)) {
            continue;  // the parade offers no options of its own
        }
        menuSubmenu(menu, scope.descriptor != nullptr ? scope.descriptor->name : "Color Picker");
        appendScopeOptions(model, scope.id, true, menu, paramActions);
        menuEndSubmenu(menu);
    }
}

void appendLayoutSubmenu(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    const LayoutOrientation current = model.view.layout().orientation();
    menuSubmenu(menu, "Layout");
    menuAction(menu, "Automatic", MenuLayoutAuto, current == LayoutOrientation::Automatic);
    menuAction(menu, "Vertical (stacked)", MenuLayoutVertical, current == LayoutOrientation::Vertical);
    menuAction(menu, "Horizontal (side by side)", MenuLayoutHorizontal, current == LayoutOrientation::Horizontal);
    menuEndSubmenu(menu);
}

void appendUiScaleSubmenu(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    // An ascending zoom-like scale of multipliers on the system scale. The 1.0
    // step is the OS's own per-monitor scaling unchanged - the home of the
    // scale, named "Default (100%)" where it sits in the middle rather than a
    // bare percentage. The checked step is an exact UiScaleSteps value, so the
    // equality is safe.
    menuSubmenu(menu, "UI Scaling");
    for (std::size_t step = 0; step < UiScaleSteps.size(); ++step) {
        const float factor = UiScaleSteps[step];
        const bool checked = factor == model.userUiScaleFactor;
        const int id = MenuUiScaleBase + static_cast<int>(step);
        if (factor == 1.0f) {
            menuAction(menu, "Default (100%)", id, checked);
        } else {
            char label[16];
            std::snprintf(label, sizeof(label), "%d%%", static_cast<int>(std::lround(factor * 100.0f)));
            menuAction(menu, label, id, checked);
        }
    }
    menuEndSubmenu(menu);
}

void appendGraticuleSubmenu(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    // How heavily the graticule is drawn, in words: a percentage states the
    // arithmetic rather than the result, and what the user is choosing is how
    // the lines read over a trace. The list stops at the floor: the graticule
    // quietens for a busy trace but never leaves, so there is no off. The
    // shipped step still says so, since a checkmark tells you where you are
    // but not where you started. The checked step is an exact
    // GraticuleStrengths value, so the equality is safe.
    menuSubmenu(menu, "Graticule");
    for (std::size_t step = 0; step < GraticuleStrengths.size(); ++step) {
        const float strength = GraticuleStrengths[step];
        const bool checked = strength == model.view.graticuleStrength();
        const int id = MenuGraticuleBase + static_cast<int>(step);
        std::string label = GraticuleStrengthNames[step];
        if (strength == DefaultGraticuleStrength) {
            label += " (default)";
        }
        menuAction(menu, label.c_str(), id, checked);
    }
    menuEndSubmenu(menu);
}

void appendQualitySubmenu(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    // One choice for how much of the machine the analysis may spend: the rate
    // the screen is read at, the resolution every scope image is computed at,
    // and how densely the region is sampled.
    menuSubmenu(menu, "Quality");
    for (std::size_t step = 0; step < QualityLevels.size(); ++step) {
        const QualityLevel level = QualityLevels[step];
        menuAction(menu, qualityLabel(level), MenuQualityBase + static_cast<int>(step), level == model.quality);
    }
    menuEndSubmenu(menu);
}

void appendPresetsSubmenu(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    // Each slot goes by its name, marked when it holds nothing yet; the digit
    // hint teaches the load shortcut. Saving rides a nested submenu with the
    // Shift+digit hint, over the same names, so the two lists read alike.
    menuSubmenu(menu, "Presets");
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const LayoutPreset& preset = model.presets[static_cast<std::size_t>(slot - 1)];
        menuAction(menu, presetLabel(slot, preset).c_str(), MenuLoadPresetBase + slot, slot == model.activePresetSlot,
                   std::to_string(slot));
    }
    menuSeparator(menu);
    menuSubmenu(menu, "Save Current To");
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const LayoutPreset& preset = model.presets[static_cast<std::size_t>(slot - 1)];
        menuAction(menu, presetDisplayName(slot, preset).c_str(), MenuSavePresetBase + slot, false,
                   "Shift+" + std::to_string(slot));
    }
    menuEndSubmenu(menu);
    menuEndSubmenu(menu);
}

void appendRegionAndAppSection(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    menuSeparator(menu);
    menuAction(menu, "Attach to Window...", MenuAttachWindow, false,
               shortcutLabel(model.shortcuts.bindings().attachWindow));
    menuAction(menu, "Draw Region...", MenuDrawRegion, false, shortcutLabel(model.shortcuts.bindings().drawRegion));
    if (supportsFaceDetection()) {
        menuAction(menu, "Attach to Face...", MenuAttachFace, false,
                   shortcutLabel(model.shortcuts.bindings().attachFace));
    }
    if (model.regionSelected) {
        menuAction(menu, "Clear Region", MenuClearRegion, false, shortcutLabel(model.shortcuts.bindings().clearRegion));
    }
    if (model.attach.attachedCount() > 1) {
        if (model.attach.activeIdentity() != 0) {
            menuAction(menu, "Detach Front Window", MenuDetachWindow, false);
        }
        menuAction(menu, "Detach All Windows", MenuDetachAll, false);
    } else if (model.attach.attached()) {
        menuAction(menu, "Detach from Window", MenuDetachWindow, false);
    }

    menuSeparator(menu);
    appendGraticuleSubmenu(model, menu);

    menuSeparator(menu);
    // Support tooling in one clearly named place; every checkbox reads the live
    // truth, so a session started by the environment shows as switched on and
    // can be switched off here. Reset restores the standard state however
    // recording or visibility were enabled.
    menuSubmenu(menu, "Diagnostics");
    if (captureVisibilityToggleSupported()) {
        menuAction(menu, "Show in Screen Captures", MenuToggleCaptureVisibility, captureVisible());
    }
    menuAction(menu, "Record Diagnostic Log", MenuToggleDiagRecording, diagRecording());
    menuAction(menu, "Show Diagnostic Log", MenuShowDiagLog, false);
    menuSeparator(menu);
    menuAction(menu, "Reset to Defaults", MenuResetDiagnostics, false);
    menuEndSubmenu(menu);
    appendQualitySubmenu(model, menu);
    appendUiScaleSubmenu(model, menu);
    menuAction(menu, "Settings", MenuOpenSettings, false);
    menuAction(menu, "About SideScopes", MenuAbout, false);
    menuAction(menu, "Quit", MenuQuit, false);
}

}  // namespace

std::string presetLabel(int slot, const LayoutPreset& preset)
{
    const std::string name = presetDisplayName(slot, preset);

    return preset.stack.empty() ? name + " (empty)" : name;
}

void buildContextMenu(const ContextMenuModel& model, int clickedPane, std::vector<NativeMenuItem>& menu,
                      std::vector<ParamMenuAction>& paramActions)
{
    // One rule shapes the menu: ownership shows through position and grouping.
    // The clicked pane's options lead, unprefixed - the click is the context; a
    // background or toolbar click wraps each scope's options in its own submenu.
    if (clickedPane >= 0) {
        const std::string& clickedId = model.registry.scopes()[static_cast<std::size_t>(clickedPane)].id;
        if (scopeHasOptions(model.registry, clickedId)) {
            appendScopeOptions(model, clickedId, false, menu, paramActions);
            menuSeparator(menu);
        }
    }
    appendScopesSubmenu(model, menu);
    if (clickedPane < 0) {
        appendPerScopeOptions(model, menu, paramActions);
    }
    appendLayoutSubmenu(model, menu);
    appendPresetsSubmenu(model, menu);
    appendRegionAndAppSection(model, menu);
}

namespace {

// The zoom entries set their level outright, where the key steps a cycle.
ShortcutAction zoomAction(int level)
{
    ShortcutAction action = ShortcutAction::plain(ShortcutAction::Kind::SetZoom);
    action.zoomLevel = level;

    return action;
}

// The preset slots, whose two ranges each map an id back to a slot. Kept apart
// from the fixed ids so neither reading has to carry the other's shape.
std::optional<ShortcutAction> presetAction(int chosen)
{
    if (chosen > MenuLoadPresetBase && chosen <= MenuLoadPresetBase + LayoutPresetSlots) {
        return ShortcutAction::preset(chosen - MenuLoadPresetBase, false);
    }
    if (chosen > MenuSavePresetBase && chosen <= MenuSavePresetBase + LayoutPresetSlots) {
        return ShortcutAction::preset(chosen - MenuSavePresetBase, true);
    }

    return std::nullopt;
}

// Opens the folder holding the diagnostic log, so "send the log" is a click
// instead of a hunt through the temp directory.
void openDiagLogFolder()
{
    std::string folder = diagLogPath();
    std::replace(folder.begin(), folder.end(), '\\', '/');
    const std::size_t cut = folder.find_last_of('/');
    if (cut == std::string::npos) {
        return;  // a bare file name names no folder to show
    }
    folder.resize(cut == 0 ? 1 : cut);  // a file at the root keeps the root
    const std::string url = (folder.front() == '/' ? "file://" : "file:///") + folder;
    openUrl(url.c_str());
}

}  // namespace

const ParamMenuAction* menuScopeParam(int chosen, const std::vector<ParamMenuAction>& paramActions)
{
    if (chosen < ParamMenuActionBase) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(chosen - ParamMenuActionBase);

    return index < paramActions.size() ? &paramActions[index] : nullptr;
}

std::optional<std::string> menuScopeToggle(int chosen, const ScopeRegistry& registry)
{
    // The scope-toggle ids carry the scope's registry index, so this resolves
    // any registered scope without naming one.
    const int index = chosen - MenuShowScopeBase;
    const std::vector<HostScope>& scopes = registry.scopes();
    if (index < 0 || index >= static_cast<int>(scopes.size())) {
        return std::nullopt;
    }

    return scopes[static_cast<std::size_t>(index)].id;
}

std::optional<ShortcutAction> menuShortcutAction(int chosen)
{
    switch (chosen) {
    case MenuAttachWindow:
        return ShortcutAction::pick(RegionPickerMode::AttachWindow);
    case MenuDrawRegion:
        return ShortcutAction::pick(RegionPickerMode::DrawGlobal);
    case MenuAttachFace:
        return ShortcutAction::pick(RegionPickerMode::AttachFace);
    case MenuPinColor:
        return ShortcutAction::pick(RegionPickerMode::PinColor);
    // Detach All clears every region, which is what clearing does now that
    // there is no whole-display state left to fall back to.
    case MenuClearRegion:
    case MenuDetachAll:
        return ShortcutAction::plain(ShortcutAction::Kind::ClearRegion);
    case MenuOpenSettings:
        return ShortcutAction::plain(ShortcutAction::Kind::OpenSettings);
    case MenuQuit:
        return ShortcutAction::plain(ShortcutAction::Kind::QuitWindow);
    case MenuZoom1:
        return zoomAction(1);
    case MenuZoom2:
        return zoomAction(2);
    case MenuZoom4:
        return zoomAction(4);
    default:
        break;
    }

    return presetAction(chosen);
}

std::optional<float> menuGraticuleStrength(int chosen)
{
    const int step = chosen - MenuGraticuleBase;
    if (step < 0 || step >= static_cast<int>(GraticuleStrengths.size())) {
        return std::nullopt;
    }

    return GraticuleStrengths[static_cast<std::size_t>(step)];
}

std::optional<LayoutOrientation> menuOrientation(int chosen)
{
    switch (chosen) {
    case MenuLayoutAuto:
        return LayoutOrientation::Automatic;
    case MenuLayoutVertical:
        return LayoutOrientation::Vertical;
    case MenuLayoutHorizontal:
        return LayoutOrientation::Horizontal;
    default:
        return std::nullopt;
    }
}

std::optional<int> menuUiScaleStep(int chosen)
{
    const int step = chosen - MenuUiScaleBase;

    return step >= 0 && step < static_cast<int>(UiScaleSteps.size()) ? std::optional<int>{step} : std::nullopt;
}

std::optional<QualityLevel> menuQuality(int chosen)
{
    const int step = chosen - MenuQualityBase;
    if (step < 0 || step >= static_cast<int>(QualityLevels.size())) {
        return std::nullopt;
    }

    return QualityLevels[static_cast<std::size_t>(step)];
}

bool applyDiagnosticsMenu(int chosen)
{
    switch (chosen) {
    case MenuToggleCaptureVisibility:
        setCaptureVisibility(!captureVisible());

        return true;
    case MenuToggleDiagRecording:
        // The menu records everything; channel selection stays with the
        // SIDESCOPES_DIAG environment for development use.
        diagConfigure(diagRecording() ? DiagConfig{} : DiagConfig{"all", "", DiagFlush::Interval});

        return true;
    case MenuShowDiagLog:
        openDiagLogFolder();

        return true;
    case MenuResetDiagnostics:
        setCaptureVisibility(false);
        if (diagRecording()) {
            diagConfigure(DiagConfig{});
        }

        return true;
    default:
        return false;
    }
}

}  // namespace sidescopes
