#pragma once

#include <string>
#include <vector>

namespace sidescopes {

/// Declarative native context menu. Blocking: shows the menu at the cursor
/// and returns the chosen action id, or -1 when dismissed. Menus are the most
/// native-feeling surface an app can offer and cost one small implementation
/// per platform.
struct NativeMenuItem
{
    enum class Kind
    {
        Action,
        Separator,
        SubmenuBegin,
        SubmenuEnd
    };
    Kind kind = Kind::Action;
    std::string label;
    int actionId = -1;
    bool checked = false;
    /// Display-only keyboard shortcut, shown the platform's way: a single
    /// letter, optionally prefixed "Shift+", or "Esc". The application
    /// handles the keys itself; the menu only teaches them.
    std::string shortcut;
};

int showNativeContextMenu(const std::vector<NativeMenuItem>& items);

/// Whether this platform has a native context menu to show. Where it does
/// not - Linux, which offers no shared menu service, and a browser, which
/// has no desktop at all - showNativeContextMenu reports dismissal
/// immediately and the application draws the same items itself with ImGui.
[[nodiscard]] bool nativeContextMenuAvailable();

}  // namespace sidescopes
