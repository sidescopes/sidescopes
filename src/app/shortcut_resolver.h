#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/preferences.h"
#include "platform/desktop.h"
#include "platform/region_selection.h"

namespace sidescopes {

class ScopeRegistry;

/// What a shortcut asks the shell to carry out. The resolver decides which
/// action a key means and fills the fields that action reads; the shell only
/// executes it. @c Kind::None is the answer for a key that matched nothing.
struct ShortcutAction
{
    enum class Kind
    {
        None,
        /// Show a scope: alone, or alongside the others when Shift composed
        /// the press. The keyboard chooses where the menu toggles.
        ChooseScope,
        /// Open one of the region tools.
        RequestPick,
        /// The zoom key steps around its cycle; the level it landed on rides
        /// in @c zoomLevel. The menu sets its levels outright.
        SetZoom,
        /// The full-screen key peels one layer: the settings window first,
        /// and the regions only when nothing is stacked above them.
        CloseSettings,
        ResetToFullScreen,
        LoadPreset,
        SavePreset,
        HideApplication,
        MinimizeWindow,
        QuitWindow,
        OpenSettings,
    };

    /// A pick request for @p mode, as the region keys and the region menu both
    /// ask for it.
    [[nodiscard]] static ShortcutAction pick(RegionPickerMode mode);

    /// Layout preset @p slot, saved when @p save and loaded otherwise, as the
    /// digit keys and the layout menu both ask for it.
    [[nodiscard]] static ShortcutAction preset(int slot, bool save);

    /// An action carrying nothing beyond its kind.
    [[nodiscard]] static ShortcutAction plain(Kind kind);

    Kind kind = Kind::None;
    std::string scopeId;                                         ///< ChooseScope: the scope to show.
    bool stack = false;                                          ///< ChooseScope: Shift stacks.
    RegionPickerMode pickMode = RegionPickerMode::AttachWindow;  ///< RequestPick: which tool opens.
    int presetSlot = 0;                                          ///< Load/SavePreset: the slot, 1-9.
    int zoomLevel = 1;                                           ///< SetZoom: the level to apply.
};

/// The shell state one resolution reads. All of it changes between frames or
/// between platforms, so it travels with the call instead of being held: the
/// resolver never reaches back into the shell.
struct ShortcutContext
{
    /// A text field holds the keyboard, so every shortcut stands down.
    bool wantsTextInput = false;
    /// Without face detection the face tool has nothing to offer, and its key
    /// stays silent; likewise the pin key without a scope that takes pins.
    bool faceDetectionSupported = false;
    bool pinsAvailable = false;
    /// The settings window is up, so the full-screen key closes that first.
    bool settingsOpen = false;
    /// The live vectorscope zoom the cycle steps on from.
    int vectorscopeZoom = 1;
    // The window chords the platform decides rather than the preferences,
    // from the platform seams of the same names.
    bool hidesWindowOnCommandW = false;
    bool minimizesWindowOnControlW = false;
    bool quitsOnControlQ = false;
};

/// Answers whether the key a shortcut names went down this frame. The name is
/// what a binding holds - a single letter A-Z or "Escape" - plus the two the
/// resolver names for itself: a preset digit "1"-"9", and "Comma" for the
/// settings chord. Injected, so the whole mapping can be judged without a
/// toolkit.
using ShortcutKeyPressed = std::function<bool(std::string_view)>;

/// Owns the keyboard bindings and maps a key to the action it means: the
/// per-scope letters, the region tools, the zoom cycle, the full-screen peel,
/// the layout-preset digits, and the platform window chords. It carries
/// nothing out itself - the shell applies the ShortcutAction it returns - and
/// reads only the registry it is constructed with and the context each call
/// carries. Both ways in share one mapping: the per-frame scan over a key
/// probe, and the by-name resolution the region border's forwarded keys take.
class ShortcutResolver
{
public:
    /// @p registry names the scopes a letter can resolve to; it must outlive
    /// the resolver.
    explicit ShortcutResolver(const ScopeRegistry& registry);

    /// Adopts the bindings from preferences: @p bindings the action keys,
    /// @p scopeOverrides the per-scope-id overrides a file names.
    void restore(const ShortcutBindings& bindings, std::map<std::string, std::string> scopeOverrides);

    /// The action bindings, for persistence and for the labels the menu
    /// entries and the tool tooltips wear.
    [[nodiscard]] const ShortcutBindings& bindings() const;

    /// The per-scope overrides, for persistence.
    [[nodiscard]] const std::map<std::string, std::string>& scopeOverrides() const;

    /// A scope's key: the user's override when set, otherwise the scope's
    /// registry letter, or empty when it has none.
    [[nodiscard]] std::string bindingFor(std::string_view id) const;

    /// The per-frame scan: an action for every bound key @p pressed reports
    /// down, in the order the shell has always checked them - the scope
    /// letters, the region tools, the view keys, then the preset digits. One
    /// frame carries as many presses as the event queue drained while the loop
    /// was blocked, and a press is edge-triggered, so dropping the ones after
    /// the first would lose them for good. The window chords go first and
    /// answer alone; any Command, Control, or Option chord silences the plain
    /// keys. @p modifiers is the live modifier state - Shift alone stays
    /// meaningful, and stacks.
    [[nodiscard]] std::vector<ShortcutAction> resolvePressed(const ShortcutContext& context,
                                                             const ModifierState& modifiers,
                                                             const ShortcutKeyPressed& pressed) const;

    /// The same mapping by name, for the keys the region border panel
    /// forwards: @p key is a binding's key and @p shift whether Shift composed
    /// it. The window chords are not reachable this way.
    [[nodiscard]] ShortcutAction resolveNamed(const std::string& key, bool shift, const ShortcutContext& context) const;

    /// The level the zoom key steps to from @p current: 1, 2, 4, and around
    /// again.
    [[nodiscard]] static int cycledZoom(int current);

private:
    [[nodiscard]] std::vector<ShortcutAction> resolvePlainKeys(const ShortcutContext& context, bool shift,
                                                               const ShortcutKeyPressed& pressed) const;
    void appendScopeKeys(bool shift, const ShortcutKeyPressed& pressed, std::vector<ShortcutAction>& actions) const;

    const ScopeRegistry& m_registry;
    ShortcutBindings m_bindings;
    std::map<std::string, std::string> m_scopeOverrides;
};

}  // namespace sidescopes
