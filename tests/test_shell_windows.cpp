// Unit tests for the shell window classes (shell_windows.cpp): which
// Windows window classes belong to the shell rather than to an
// application. The classes are pinned by name here because the Windows
// desktop layer that reads them cannot be exercised off Windows, and a
// misspelt class silently admits the window it exists to exclude.

#include <catch2/catch_test_macros.hpp>

#include "platform/shell_windows.h"

namespace sidescopes {

TEST_CASE("The focus transition surfaces are named")
{
    // Every switcher host: a foreground change into one of these is a
    // switch in flight, not a window the user has moved to.
    CHECK(isFocusTransitionWindowClass(L"XamlExplorerHostIslandWindow"));
    CHECK(isFocusTransitionWindowClass(L"MultitaskingViewFrame"));
    CHECK(isFocusTransitionWindowClass(L"ForegroundStaging"));
    CHECK(isFocusTransitionWindowClass(L"TaskSwitcherWnd"));
    CHECK(isFocusTransitionWindowClass(L"TaskSwitcherOverlayWnd"));
}

TEST_CASE("The desktop and the taskbars are shell windows")
{
    // Progman is the one that matters: the desktop is visible, titled
    // "Program Manager", uncloaked, owned by a real process and as large
    // as the display, so no other listing rule rejects it.
    CHECK(isShellOwnedWindowClass(L"Progman"));
    CHECK(isShellOwnedWindowClass(L"WorkerW"));
    CHECK(isShellOwnedWindowClass(L"Shell_TrayWnd"));
    CHECK(isShellOwnedWindowClass(L"Shell_SecondaryTrayWnd"));
    CHECK(isShellOwnedWindowClass(L"NotifyIconOverflowWindow"));
    CHECK(isShellOwnedWindowClass(L"TopLevelWindowForOverflowXamlIsland"));
}

TEST_CASE("A shell window includes every focus transition surface")
{
    // The switcher hosts are shell windows too: what may not reroute the
    // region may not be scoped either.
    CHECK(isShellOwnedWindowClass(L"XamlExplorerHostIslandWindow"));
    CHECK(isShellOwnedWindowClass(L"MultitaskingViewFrame"));
    CHECK(isShellOwnedWindowClass(L"ForegroundStaging"));
    CHECK(isShellOwnedWindowClass(L"TaskSwitcherWnd"));
    CHECK(isShellOwnedWindowClass(L"TaskSwitcherOverlayWnd"));
}

TEST_CASE("Application window classes are not shell windows")
{
    // The ordinary top-level classes an editor, a browser and a store app
    // register, plus this application's own: none may be filtered out of a
    // listing.
    CHECK_FALSE(isShellOwnedWindowClass(L"ApplicationFrameWindow"));
    CHECK_FALSE(isShellOwnedWindowClass(L"Windows.UI.Core.CoreWindow"));
    CHECK_FALSE(isShellOwnedWindowClass(L"CabinetWClass"));  // a File Explorer window
    CHECK_FALSE(isShellOwnedWindowClass(L"Chrome_WidgetWin_1"));
    CHECK_FALSE(isShellOwnedWindowClass(L"GLFW30"));
    CHECK_FALSE(isFocusTransitionWindowClass(L"CabinetWClass"));
}

TEST_CASE("A class name matches whole and exactly")
{
    // GetClassNameW hands back the registered spelling, so the comparison
    // is exact: neither a prefix of a shell class nor a class that merely
    // starts with one may match.
    CHECK_FALSE(isShellOwnedWindowClass(L""));
    CHECK_FALSE(isShellOwnedWindowClass(L"Prog"));
    CHECK_FALSE(isShellOwnedWindowClass(L"ProgmanHost"));
    CHECK_FALSE(isShellOwnedWindowClass(L"progman"));
    CHECK_FALSE(isShellOwnedWindowClass(L"Shell_TrayWndEx"));
}

}  // namespace sidescopes
