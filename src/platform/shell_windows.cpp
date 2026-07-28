#include "platform/shell_windows.h"

#include <array>

namespace sidescopes {
namespace {

constexpr std::array<std::wstring_view, 5> FocusTransitionClasses{
    L"XamlExplorerHostIslandWindow",  // Windows 11 alt-tab and task view
    L"MultitaskingViewFrame",         // Windows 10 alt-tab and task view
    L"ForegroundStaging",             // transient staging between switches
    L"TaskSwitcherWnd",               // classic alt-tab
    L"TaskSwitcherOverlayWnd",
};

constexpr std::array<std::wstring_view, 6> ShellChromeClasses{
    L"Progman",                              // the desktop, titled "Program Manager"
    L"WorkerW",                              // the desktop's wallpaper host
    L"Shell_TrayWnd",                        // the taskbar
    L"Shell_SecondaryTrayWnd",               // a second display's taskbar
    L"NotifyIconOverflowWindow",             // the tray overflow through Windows 10
    L"TopLevelWindowForOverflowXamlIsland",  // the same on Windows 11
};

}  // namespace

bool isFocusTransitionWindowClass(std::wstring_view className)
{
    for (const std::wstring_view known : FocusTransitionClasses) {
        if (className == known) {
            return true;
        }
    }

    return false;
}

bool isShellOwnedWindowClass(std::wstring_view className)
{
    if (isFocusTransitionWindowClass(className)) {
        return true;
    }
    for (const std::wstring_view known : ShellChromeClasses) {
        if (className == known) {
            return true;
        }
    }

    return false;
}

}  // namespace sidescopes
