#pragma once

#include <string_view>

namespace sidescopes {

/// Which Windows window classes belong to the shell rather than to an
/// application. The knowledge is Windows-only; the rule lives here, free
/// of Win32, so the class names are pinned by tests on every platform
/// instead of being rediscovered against a live desktop.

/// Whether @p className names one of the surfaces that take the foreground
/// mid focus switch - the alt-tab and task-view hosts, and the staging
/// window foreground changes pass through. The user works in none of them.
[[nodiscard]] bool isFocusTransitionWindowClass(std::wstring_view className);

/// Whether @p className names a window the shell owns: the desktop, the
/// taskbars, the tray overflow, and the focus transition surfaces above.
/// None of them is a window to scope, and the desktop is the one that has
/// to be named rather than filtered - it is visible, titled, uncloaked and
/// display-sized, so every other listing rule admits it.
[[nodiscard]] bool isShellOwnedWindowClass(std::wstring_view className);

}  // namespace sidescopes
