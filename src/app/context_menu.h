#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "app/attach_controller.h"
#include "app/param_menu.h"
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
    MenuShowVectorscope = 1,
    MenuShowWaveform,
    MenuShowWaveformParade,
    MenuShowHistogram,
    MenuShowColorPicker,
    MenuDrawRegion = 25,
    MenuAttachFace,
    MenuZoom1,
    MenuZoom2,
    MenuZoom4,
    MenuAttachWindow = 30,
    MenuFullScreen,
    MenuDetachWindow,
    MenuDetachAll,
    MenuToggleGraticule = 40,
    MenuClearPinnedMarkers,
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
};

/// The read-only snapshot the context-menu builder reads from the app: the view
/// and registry it reflects, the shortcut labels, the pin and attach state, the
/// preset slots, and the interface-size factor. References stay valid for the
/// single synchronous build call.
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
    bool isFullScreen;
};

/// A preset slot's menu label: "N - empty" for an unused slot, otherwise a
/// short summary like "1 - VWH", naming the orientation only when the preset
/// pins one.
[[nodiscard]] std::string presetLabel(int slot, const LayoutPreset& preset);

/// Builds the right-click menu for @p clickedPane (-1 = a background or toolbar
/// click) from @p model, filling @p menu and the dynamic scope-parameter side
/// table @p paramActions that @ref dispatchMenuChoice resolves against.
void buildContextMenu(const ContextMenuModel& model, int clickedPane, std::vector<NativeMenuItem>& menu,
                      std::vector<ParamMenuAction>& paramActions);

}  // namespace sidescopes
