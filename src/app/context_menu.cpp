#include "app/context_menu.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <map>
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

std::string shortcutLabel(const std::string& name)
{
    return name == "Escape" ? "Esc" : name;
}

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

// The lowercase orientation word for a menu summary line.
const char* orientationName(LayoutOrientation orientation)
{
    switch (orientation) {
    case LayoutOrientation::Vertical:
        return "vertical";
    case LayoutOrientation::Horizontal:
        return "horizontal";
    case LayoutOrientation::Automatic:
    default:
        return "auto";
    }
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
    menuAction(menu, "Pin Colors...", MenuPinColor, false, shortcutLabel(model.shortcuts.pinColor));
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
    menuAction(menu, "1x", MenuZoom1, model.view.zoom() == 1, shortcutLabel(model.shortcuts.vectorscopeZoom));
    menuAction(menu, "2x", MenuZoom2, model.view.zoom() == 2, shortcutLabel(model.shortcuts.vectorscopeZoom));
    menuAction(menu, "4x", MenuZoom4, model.view.zoom() == 4, shortcutLabel(model.shortcuts.vectorscopeZoom));
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
    menuAction(menu, "Vectorscope", MenuShowVectorscope, model.view.shows(VectorscopeScopeId),
               shortcutLabel(resolveBinding(model.scopeShortcuts, model.registry, VectorscopeScopeId)));
    menuAction(menu, "Waveform", MenuShowWaveform, model.view.shows(WaveformScopeId),
               shortcutLabel(resolveBinding(model.scopeShortcuts, model.registry, WaveformScopeId)));
    menuAction(menu, "RGB Parade", MenuShowWaveformParade, model.view.shows(ParadeScopeId),
               shortcutLabel(resolveBinding(model.scopeShortcuts, model.registry, ParadeScopeId)));
    menuAction(menu, "Histogram", MenuShowHistogram, model.view.shows(HistogramScopeId),
               shortcutLabel(resolveBinding(model.scopeShortcuts, model.registry, HistogramScopeId)));
    menuAction(menu, "Color Picker", MenuShowColorPicker, model.view.shows(ColorPickerScopeId),
               shortcutLabel(resolveBinding(model.scopeShortcuts, model.registry, ColorPickerScopeId)));
    menuEndSubmenu(menu);
}

void appendPerScopeOptions(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu,
                           std::vector<ParamMenuAction>& paramActions)
{
    // On a background or toolbar click, each visible scope's options ride under
    // its own name, in toolbar order.
    for (const HostScope& scope : model.registry.scopes()) {
        if (!model.view.shows(scope.id)) {
            continue;
        }
        // The vectorscope's section already carries the pins; the color picker
        // shows them only when the vectorscope is gone.
        if (scope.id == ColorPickerScopeId && model.view.shows(VectorscopeScopeId)) {
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
    const LayoutOrientation current = model.view.orientation();
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

void appendPresetsSubmenu(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    // Each slot lists its saved summary or "empty"; the digit hint teaches the
    // load shortcut. Saving rides a nested submenu with the Shift+digit hint.
    menuSubmenu(menu, "Presets");
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const LayoutPreset& preset = model.presets[static_cast<std::size_t>(slot - 1)];
        menuAction(menu, presetLabel(slot, preset).c_str(), MenuLoadPresetBase + slot, slot == model.activePresetSlot,
                   std::to_string(slot));
    }
    menuSeparator(menu);
    menuSubmenu(menu, "Save Current To");
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        menuAction(menu, std::to_string(slot).c_str(), MenuSavePresetBase + slot, false,
                   "Shift+" + std::to_string(slot));
    }
    menuEndSubmenu(menu);
    menuEndSubmenu(menu);
}

void appendRegionAndAppSection(const ContextMenuModel& model, std::vector<NativeMenuItem>& menu)
{
    menuSeparator(menu);
    menuAction(menu, "Attach to Window...", MenuAttachWindow, false, shortcutLabel(model.shortcuts.attachWindow));
    menuAction(menu, "Draw Region...", MenuDrawRegion, false, shortcutLabel(model.shortcuts.drawRegion));
    if (supportsFaceDetection()) {
        menuAction(menu, "Attach to Face...", MenuAttachFace, false, shortcutLabel(model.shortcuts.attachFace));
    }
    menuAction(menu, "Watch Full Screen", MenuFullScreen, model.isFullScreen,
               shortcutLabel(model.shortcuts.fullScreen));
    if (model.attach.attachedCount() > 1) {
        if (model.attach.activeIdentity() != 0) {
            menuAction(menu, "Detach Front Window", MenuDetachWindow, false);
        }
        menuAction(menu, "Detach All Windows", MenuDetachAll, false);
    } else if (model.attach.attached()) {
        menuAction(menu, "Detach from Window", MenuDetachWindow, false);
    }

    menuSeparator(menu);
    menuAction(menu, "Graticule", MenuToggleGraticule, model.view.graticule());

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
    appendUiScaleSubmenu(model, menu);
    menuAction(menu, "Settings", MenuOpenSettings, false);
    menuAction(menu, "About SideScopes", MenuAbout, false);
    menuAction(menu, "Quit", MenuQuit, false);
}

}  // namespace

std::string resolveBinding(const std::map<std::string, std::string>& overrides, const ScopeRegistry& registry,
                           std::string_view id)
{
    if (const auto custom = overrides.find(std::string{id}); custom != overrides.end()) {
        return custom->second;
    }
    const HostScope* scope = registry.byId(id);

    return scope != nullptr && scope->letter != 0 ? std::string(1, scope->letter) : std::string{};
}

std::string presetLabel(int slot, const LayoutPreset& preset)
{
    const std::string number = std::to_string(slot);
    if (preset.stack.empty()) {
        return number + " - empty";
    }
    std::string label = number + " - " + preset.stack;
    const LayoutOrientation orientation = orientationFromInt(preset.orientation);
    if (orientation != LayoutOrientation::Automatic) {
        label += ' ';
        label += orientationName(orientation);
    }

    return label;
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

}  // namespace sidescopes
