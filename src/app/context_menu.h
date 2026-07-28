#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/attach_controller.h"
#include "app/layout_preset_store.h"
#include "app/param_menu.h"
#include "app/quality.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"
#include "core/preferences.h"
#include "platform/native_menu.h"

namespace sidescopes {

/// Fixed ids for the host actions the right-click menu drives. Scope parameter
/// choices are dynamic: they carry ids from ParamMenuActionBase upward,
/// resolved through a per-open side table, never through this enum.
enum MenuAction
{
    MenuDrawRegion = 25,
    MenuAttachFace,
    MenuZoom1,
    MenuZoom2,
    MenuZoom4,
    MenuAttachWindow = 30,
    MenuClearRegion,
    MenuDetachWindow,
    MenuDetachAll,
    MenuClearPinnedMarkers = 41,
    MenuPinColor,
    MenuToggleCaptureVisibility,
    MenuToggleDiagRecording,
    MenuShowDiagLog,
    MenuResetDiagnostics,
    MenuOpenSettings = 50,
    MenuAbout,
    MenuQuit,
    MenuLayoutAuto = 60,
    MenuLayoutVertical,
    MenuLayoutHorizontal,
    // Preset load ids are MenuLoadPresetBase + slot (1-9); save ids are
    // MenuSavePresetBase + slot. Both ranges stay clear of ParamMenuActionBase.
    MenuLoadPresetBase = 70,
    MenuSavePresetBase = 80,
    // Interface-size ids are MenuUiScaleBase + the UiScaleSteps index.
    MenuUiScaleBase = 90,
    // Quality ids are MenuQualityBase + the QualityLevels index.
    MenuQualityBase = 97,
    // Scope-toggle ids are MenuShowScopeBase + the scope's index in the
    // registry, so every registered scope gets a menu entry with no hardcoded
    // list. Clear of ParamMenuActionBase.
    MenuShowScopeBase = 100,
    // Graticule-strength ids are MenuGraticuleBase + the GraticuleStrengths
    // index, above the scope range so a new scope cannot grow into them.
    MenuGraticuleBase = 200,
};

/// The read-only snapshot the context-menu builder reads from the app: the view
/// and registry it reflects, the shortcut labels, the pin and attach state, the
/// preset slots, the interface-size factor, and the quality level. References
/// stay valid for the single synchronous build call.
struct ContextMenuModel
{
    const ScopeView& view;
    const ScopeRegistry& registry;
    /// The keys every entry that has one is labelled with.
    const ShortcutResolver& shortcuts;
    const std::map<std::string, std::map<std::string, double>>& scopeParams;
    const AttachController& attach;
    const std::array<LayoutPreset, LayoutPresetSlots>& presets;
    bool pinsEmpty;
    int activePresetSlot;
    float userUiScaleFactor;
    /// How much of the machine the analysis may spend.
    QualityLevel quality;
    /// Whether a region has been selected at all; without one there is nothing
    /// for Clear Region to clear.
    bool regionSelected;
};

/// A preset slot's menu label: what the slot is called, marked "(empty)" while
/// it holds no layout. The digit that loads it is the entry's shortcut, not
/// part of its name.
[[nodiscard]] std::string presetLabel(int slot, const LayoutPreset& preset);

/// Builds the right-click menu for @p clickedPane (-1 = a background or toolbar
/// click) from @p model, filling @p menu and the dynamic scope-parameter side
/// table @p paramActions that @ref menuScopeParam resolves against.
void buildContextMenu(const ContextMenuModel& model, int clickedPane, std::vector<NativeMenuItem>& menu,
                      std::vector<ParamMenuAction>& paramActions);

// What a chosen id means. Every one of these is the inverse of a range this
// file lays out above, so the arithmetic that encodes a choice and the
// arithmetic that reads it back sit together, under the same static_asserts
// that keep the ranges from growing into each other. A choice belongs to at
// most one of them: the ranges are disjoint by construction.

/// @return The scope parameter @p chosen sets, resolved against the per-open
///         side table @p paramActions, or null when it is no such choice.
[[nodiscard]] const ParamMenuAction* menuScopeParam(int chosen, const std::vector<ParamMenuAction>& paramActions);

/// @return The id of the scope @p chosen toggles, or nothing.
[[nodiscard]] std::optional<std::string> menuScopeToggle(int chosen, const ScopeRegistry& registry);

/// @return The action @p chosen shares with a keyboard shortcut - the region
///         tools, the zoom levels, the preset slots, settings and quit - or
///         nothing. The keys reach these through the resolver, which decides
///         what a key means; a menu entry says outright which action it is.
[[nodiscard]] std::optional<ShortcutAction> menuShortcutAction(int chosen);

/// @return The graticule strength @p chosen names, or nothing.
[[nodiscard]] std::optional<float> menuGraticuleStrength(int chosen);

/// @return The layout split @p chosen names, or nothing.
[[nodiscard]] std::optional<LayoutOrientation> menuOrientation(int chosen);

/// @return The interface-size step @p chosen names, or nothing.
[[nodiscard]] std::optional<int> menuUiScaleStep(int chosen);

/// @return The quality level @p chosen names, or nothing.
[[nodiscard]] std::optional<QualityLevel> menuQuality(int chosen);

/// Carries out the diagnostics entries, which reach nothing in the shell: the
/// recorder, the capture's own visibility, and the folder the log sits in.
/// @return Whether @p chosen was one of them.
bool applyDiagnosticsMenu(int chosen);

}  // namespace sidescopes
