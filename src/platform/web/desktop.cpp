// The desktop seams, answered honestly for a browser.
//
// None of these is a placeholder waiting to be filled in. A page cannot
// enumerate another program's windows, read the screen, or move a border
// around on the desktop, and no browser API is ever going to let it: that
// is the security model working, not a gap in this file.
//
// So the demo is the INSTRUMENT rather than the application. Every seam
// here reports "nothing", which is what the callers are already written to
// handle — the region tools stand down on their own when the platform
// offers them nothing, exactly as they do on a desktop with no displays
// attached.

#include "platform/desktop.h"

#include <emscripten/console.h>
#include <emscripten/em_js.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/interface_diagnostics.h"
#include "core/frame.h"

namespace sidescopes {
namespace {

// The live modifier state comes from the page's own events, kept in one
// integer. Bit per modifier, so JavaScript hands back a single value rather
// than four calls across the boundary.
constexpr int ShiftBit = 1;
constexpr int ControlBit = 2;
constexpr int OptionBit = 4;
constexpr int CommandBit = 8;

// clang-format off
// JAVASCRIPT, not C++ - see the note in web/demo_storage.cpp. clang-format
// reads these bodies as C++ and will rewrite operators inside them.
//
// PLAIN addEventListener, deliberately, and NOT the html5.h
// emscripten_set_*_callback family. Emscripten keeps ONE callback per
// (target, event, capture phase), so registering keydown on the window
// REPLACES whatever GLFW registered there - which silently took the whole
// keyboard away from the interface toolkit. A DOM listener is additive and
// cannot collide with anything.
EM_JS(void, jsWatchModifiers, (), {
    if (window.__sidescopesModifiers !== undefined) {
        return;
    }
    window.__sidescopesModifiers = 0;
    const note = (event) => {
        window.__sidescopesModifiers =
            (event.shiftKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) |
            (event.altKey ? 4 : 0) | (event.metaKey ? 8 : 0);
    };
    // Capture phase so the value is current before any handler that acts on
    // it, and passive so this can never affect what the page does with the
    // event. Every keyboard and mouse event carries all four flags, so a
    // modifier released while the page was unfocused - which sends no keyup
    // - is corrected by the next event of any kind.
    for (const name of ["keydown", "keyup", "mousemove", "mousedown"]) {
        window.addEventListener(name, note, {capture: true, passive: true});
    }
});

EM_JS(int, jsReadModifiers, (), {
    return window.__sidescopesModifiers || 0;
});

// clang-format on

void ensureModifierTracking()
{
    static const bool watching = [] {
        jsWatchModifiers();

        return true;
    }();
    (void)watching;
}

}  // namespace

// --- what a page cannot see -------------------------------------------

bool supportsWindowAttach()
{
    return false;
}

std::vector<DesktopWindow> onScreenWindows(uint32_t)
{
    return {};
}

std::optional<WindowGeometry> windowGeometry(uint64_t)
{
    return std::nullopt;
}

std::optional<DesktopPoint> globalCursorPosition()
{
    // The pointer is known inside the canvas and nowhere else; the demo
    // reads it through Dear ImGui rather than through this seam.
    return std::nullopt;
}

std::optional<DisplayGeometry> geometryOfDisplay(uint32_t)
{
    return std::nullopt;
}

std::optional<uint32_t> displayAtPoint(DesktopPoint)
{
    return std::nullopt;
}

std::optional<uint32_t> displayUnderCursor()
{
    return std::nullopt;
}

std::vector<DesktopWindow> attachCandidateWindows(uint32_t)
{
    return {};
}

std::optional<uint64_t> focusedAttachedWindow(int64_t, const std::vector<uint64_t>&)
{
    return std::nullopt;
}

void raiseWindow(uint64_t, int64_t)
{
}

int64_t ownApplicationPid()
{
    // A page is not a process it can name, and nothing here compares this
    // against another window's owner because no other window is visible.
    return 0;
}

int64_t foregroundApplicationPid()
{
    return 0;
}

bool captureExclusionDisabled()
{
    // Nothing captures this, so nothing can be excluded from a capture.
    return false;
}

std::optional<CapturedImage> captureDisplayImage(uint32_t)
{
    // The frame comes from the page — a bundled photograph, or a file the
    // visitor dropped in — and is pushed into the analysis directly. It
    // never arrives through a screen capture, because there is none.
    return std::nullopt;
}

void sampleScreenColorAsync(DesktopPoint, std::function<void(std::optional<FloatColor>)> callback)
{
    const std::function<void(std::optional<FloatColor>)> reader = std::move(callback);
    reader(std::nullopt);
}

std::string displayName(uint32_t)
{
    return {};
}

std::string preferencesFilePath()
{
    // A real path, in the bundled in-memory filesystem. The application
    // saves and loads through its own preferences code against this, exactly
    // as it does on a desktop; what differs is only that the file does not
    // survive the page, so web/demo_storage.cpp carries its contents in and
    // out of the browser's local storage around those two calls.
    //
    // Through this seam rather than a web-only accessor, so the application
    // asks one question on every platform.
    return "/prefs/preferences.txt";
}

// --- window and application state -------------------------------------

bool applicationHidden()
{
    // A page is never hidden in the sense the frame loop means: when a tab
    // is in the background the browser simply stops calling the animation
    // frame, so the loop pauses without being told.
    return false;
}

void hideApplication()
{
    // A page cannot hide itself, and one that tried would be taking the
    // reader's own window management away.
}

void rememberApplicationWindow(void*)
{
    // The desktop layers keep this to compare a picked window against the
    // application's own. There is one canvas here and nothing to confuse it
    // with.
}

// --- the notifications a page does not receive -------------------------
//
// Each of these takes a callback the application never has cause to invoke
// here, and dropping it is the honest answer rather than a gap. Sleep, wake
// and screen lock have no page-visible equivalent - a backgrounded tab
// simply stops being called, which the frame loop already handles without
// being told. Foreground changes and window motion are about OTHER
// applications' windows, which no page can see.

void observeSystemSleep(std::function<void()>)
{
}

void observeSystemWake(std::function<void()>)
{
}

void observeEscapeWithoutKeyWindow(std::function<void()>)
{
    // macOS-only: it exists because an overlay window can hold Escape while
    // no window is key. A page has no such window to lose the key to.
}

void observeForegroundChanges(std::function<void()>)
{
}

void unobserveForegroundChanges()
{
}

void watchWindowMotion(uint64_t, int64_t, std::function<void(WindowMotionSignal)>)
{
}

void unwatchWindowMotion()
{
}

void setCaptureVisibility(bool)
{
}

bool captureVisible()
{
    return true;
}

bool captureVisibilityToggleSupported()
{
    // Nothing here can be captured by anything, so there is no visibility
    // to offer a toggle for.
    return false;
}

// The platform chords: a browser owns all of these, and a page that
// intercepted them would be taking the reader's own window controls away.

bool platformHidesWindowOnCommandW()
{
    return false;
}

bool platformMinimizesWindowOnControlW()
{
    return false;
}

bool platformQuitsOnControlQ()
{
    return false;
}

ModifierState currentModifiers()
{
    // Read from the page's own event stream, not from the interface toolkit.
    // The browser is this platform's operating system, and the platform layer
    // sits below Dear ImGui here exactly as it does on macOS and Windows - so
    // this asks the same source those do, one level down, rather than asking
    // the layer above it.
    ensureModifierTracking();
    const int bits = jsReadModifiers();
    ModifierState state{};
    state.shift = (bits & ShiftBit) != 0;
    state.control = (bits & ControlBit) != 0;
    state.option = (bits & OptionBit) != 0;
    state.command = (bits & CommandBit) != 0;

    return state;
}

void openUrl(const char*)
{
    // Deliberately inert. The demo has no link to open, and a page that
    // could navigate itself from C++ would be a surprise.
}

void openScreenRecordingSettings()
{
    // There is no such setting to open, and the help page that offers it
    // never draws here: the capture seam reports permission granted.
}

// --- fonts -------------------------------------------------------------
//
// A page has no font files to enumerate, and reaching for one over the
// network would be a request the demo does not otherwise make. Both lists
// come back empty, which the startup path already handles: Dear ImGui's
// own bundled font stands in, and the picker aligns hex codes with the
// interface font rather than a monospace companion.

std::vector<std::string> interfaceFontFiles()
{
    // Bundled by the build into the in-memory filesystem. A page has no
    // system fonts to enumerate, and returning nothing left the interface
    // on Dear ImGui's built-in bitmap face - which is neither a system font
    // nor able to draw the delta the colour picker labels with.
    return {"/fonts/ui.ttf"};
}

std::vector<std::string> monospaceFontFiles()
{
    return {"/fonts/mono.ttf"};
}

float monospaceFontScale()
{
    return 1.0f;
}

// --- interface diagnostics ---------------------------------------------

void reportInterfaceError(const char* message)
{
    // The desktop builds route this into the diagnostic log, and raise a
    // window over the application in development builds. In a browser the
    // console is the log every reader already has open, so it goes there
    // and nowhere else.
    if (message != nullptr) {
        emscripten_console_error(message);
    }
}

}  // namespace sidescopes
