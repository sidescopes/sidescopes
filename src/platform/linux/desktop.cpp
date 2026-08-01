// Desktop services on Linux. Display geometry, live keyboard state, the
// pointer and the window services all come from X11/XRandR/EWMH whenever a
// display is reachable - the X11 path also serves XWayland, so it covers
// every major desktop today. What that path can SEE under XWayland is X11
// clients: a native Wayland toplevel is invisible to window enumeration and
// stops the pointer being reported while it sits under it. Where the
// isolation forbids a service outright (screen sampling), the seam reports
// the honest empty answer and the application's fallbacks carry it: those
// services return optionals or empty vectors by contract.

#include "platform/desktop.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>

#include "core/preferences.h"
#include "platform/focus_resolution.h"
#include "platform/linux/linux_session.h"
#include "platform/linux/stream_pointer.h"
#include "platform/linux/x11_displays.h"
#include "platform/linux/x11_windows.h"

namespace sidescopes {
namespace {

/// One X11 connection for the process, opened at first use. Null on a
/// pure-Wayland session without XWayland; every caller degrades on null.
/// Main thread only - Xlib serialises nothing between threads, so the motion
/// watcher opens a connection of its own rather than sharing this one.
Display* x11Display()
{
    static Display* display = XOpenDisplay(nullptr);
    return display;
}

bool x11HasRandr()
{
    Display* display = x11Display();
    if (display == nullptr) {
        return false;
    }
    static int eventBase = 0;
    static int errorBase = 0;
    static Bool present = XRRQueryExtension(display, &eventBase, &errorBase);
    return present == True;
}

bool contains(const DisplayGeometry& geometry, DesktopPoint point)
{
    return point.x >= geometry.originX && point.x < geometry.originX + geometry.widthPoints &&
           point.y >= geometry.originY && point.y < geometry.originY + geometry.heightPoints;
}

/// The pointer in root coordinates plus the modifier mask, in one round
/// trip. False only when the pointer is on another X screen entirely.
struct PointerSample
{
    DesktopPoint position;
    unsigned int modifiers = 0;
};

std::optional<PointerSample> queryPointer()
{
    Display* display = x11Display();
    if (display == nullptr) {
        return std::nullopt;
    }
    ::Window root = 0;
    ::Window child = 0;
    int rootX = 0;
    int rootY = 0;
    int childX = 0;
    int childY = 0;
    unsigned int mask = 0;
    if (XQueryPointer(display, DefaultRootWindow(display), &root, &child, &rootX, &rootY, &childX, &childY, &mask) !=
        True) {
        return std::nullopt;
    }

    return PointerSample{DesktopPoint{static_cast<double>(rootX), static_cast<double>(rootY)}, mask};
}

/// The installed observers, owned here the way every real platform layer owns
/// them. Nothing raises them yet: session sleep, wake and lock arrive over
/// D-Bus and will be wired up with the capture backend, which is what the
/// signals exist to stop and restart.
struct Observers
{
    std::function<void()> systemSleep;
    std::function<void()> systemWake;
    std::function<void()> foregroundChanged;
    std::function<void()> escapeWithoutKeyWindow;
};

Observers& observers()
{
    static Observers installed;
    return installed;
}

}  // namespace

std::vector<LinuxDisplay> connectedDisplays()
{
    std::vector<LinuxDisplay> displays;
    Display* display = x11Display();
    if (display == nullptr) {
        return displays;
    }
    if (!x11HasRandr()) {
        LinuxDisplay whole;
        whole.id = 1;
        whole.geometry.widthPoints = DisplayWidth(display, DefaultScreen(display));
        whole.geometry.heightPoints = DisplayHeight(display, DefaultScreen(display));
        whole.name = "Screen";
        displays.push_back(whole);

        return displays;
    }
    XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, DefaultRootWindow(display));
    if (resources == nullptr) {
        return displays;
    }
    for (int index = 0; index < resources->noutput; ++index) {
        XRROutputInfo* output = XRRGetOutputInfo(display, resources, resources->outputs[index]);
        if (output == nullptr) {
            continue;
        }
        if (output->connection == RR_Connected && output->crtc != None) {
            XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, resources, output->crtc);
            if (crtc != nullptr) {
                LinuxDisplay entry;
                entry.id = static_cast<uint32_t>(resources->outputs[index]);
                entry.geometry.originX = crtc->x;
                entry.geometry.originY = crtc->y;
                entry.geometry.widthPoints = crtc->width;
                entry.geometry.heightPoints = crtc->height;
                entry.name.assign(output->name, static_cast<std::size_t>(output->nameLen));
                displays.push_back(entry);
                XRRFreeCrtcInfo(crtc);
            }
        }
        XRRFreeOutputInfo(output);
    }
    XRRFreeScreenResources(resources);

    return displays;
}

bool foreignWindowsEnumerable()
{
    // The X11 lane enumerates the whole desktop. Under XWayland it
    // enumerates X clients only, and a Wayland compositor tells a client
    // nothing at all about its neighbours.
    return runningOnX11Session();
}

std::vector<DesktopWindow> onScreenWindows(uint32_t displayId)
{
    // The X11 lane sees X11 clients: under XWayland that is every application
    // the compositor runs through Xwayland, and no native Wayland toplevel -
    // those are the portal's window source to fetch, not this list's.
    return x11OnScreenWindows(x11Display(), connectedDisplays(), displayId, getpid());
}

std::optional<WindowGeometry> windowGeometry(uint64_t identity)
{
    return x11WindowGeometry(x11Display(), identity);
}

std::optional<DesktopPoint> globalCursorPosition()
{
    // The capture stream's cursor metadata first, because on a Wayland session
    // it is the only true answer. X is told where the pointer is ONLY while it
    // sits over an X surface, so the moment it crosses onto a native Wayland
    // window XQueryPointer freezes at the last place it saw - MEASURED, 450
    // samples over 45 seconds of real movement at one unchanging coordinate.
    // A live probe reading that position shows a colour that never changes.
    //
    // A pure X11 session publishes none of this and falls through to the root
    // coordinates, which are the global desktop space directly.
    if (const std::optional<DesktopPoint> streamed = streamPointer()) {
        return streamed;
    }
    const std::optional<PointerSample> pointer = queryPointer();
    if (!pointer) {
        return std::nullopt;
    }

    return pointer->position;
}

std::optional<DisplayGeometry> geometryOfDisplay(uint32_t displayId)
{
    for (const LinuxDisplay& display : connectedDisplays()) {
        if (display.id == displayId) {
            return display.geometry;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> displayAtPoint(DesktopPoint point)
{
    for (const LinuxDisplay& display : connectedDisplays()) {
        if (contains(display.geometry, point)) {
            return display.id;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> displayUnderCursor()
{
    const std::optional<DesktopPoint> cursor = globalCursorPosition();
    if (!cursor) {
        return std::nullopt;
    }

    return displayAtPoint(*cursor);
}

void prepareNativeThreading()
{
    // The face-scan threads read the screen through Xlib while the main thread
    // drives its own connections; XInitThreads makes that concurrent use safe.
    // Its own contract requires it to precede every other Xlib call, which is
    // why main() calls this before any windowing is created.
    XInitThreads();
}

std::string preferencesFilePath()
{
    std::string elsewhere = preferencesFileFromEnvironment();
    if (!elsewhere.empty()) {
        return elsewhere;
    }
    const char* configHome = std::getenv("XDG_CONFIG_HOME");
    if (configHome != nullptr && configHome[0] != '\0') {
        return std::string(configHome) + "/sidescopes/preferences.txt";
    }
    const char* home = std::getenv("HOME");
    std::string base = home != nullptr ? home : ".";

    return base + "/.config/sidescopes/preferences.txt";
}

void openScreenRecordingSettings()
{
    // Capture consent lives in the portal dialog, not a settings pane.
}

void openUrl(const char* url)
{
    // Double fork so the browser is inherited by init instead of lingering
    // as a zombie of this process.
    pid_t child = fork();
    if (child == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            execlp("xdg-open", "xdg-open", url, static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(grandchild > 0 ? 0 : 127);
    }
    if (child > 0) {
        waitpid(child, nullptr, 0);
    }
}

ModifierState currentModifiers()
{
    ModifierState state;
    const std::optional<PointerSample> pointer = queryPointer();
    if (pointer) {
        state.shift = (pointer->modifiers & ShiftMask) != 0;
        state.control = (pointer->modifiers & ControlMask) != 0;
        state.option = (pointer->modifiers & Mod1Mask) != 0;
        state.command = (pointer->modifiers & Mod4Mask) != 0;
    }

    return state;
}

bool platformHidesWindowOnCommandW()
{
    return false;
}

bool platformMinimizesWindowOnControlW()
{
    return true;
}

bool platformQuitsOnControlQ()
{
    return true;
}

void hideApplication()
{
    // Never reached: the dismissal chord resolves to MinimizeWindow here, and
    // the application iconifies through GLFW itself.
}

bool applicationHidden()
{
    return false;
}

void observeSystemSleep(std::function<void()> callback)
{
    observers().systemSleep = std::move(callback);
}

void watchWindowMotion(uint64_t identity, int64_t, std::function<void(WindowMotionSignal)> callback)
{
    // ONLY Moved is ever delivered here. The grip pair states whether a
    // human's button is down on a FOREIGN window, which X11 reports to nobody
    // but that window's own client short of a pointer grab - and grabbing the
    // pointer away from the application the user is dragging is not a price
    // this instrument pays. So the border hides on the first geometry change
    // rather than at the grip, and comes back on the settle timer.
    x11WatchWindowMotion(x11Display(), identity, std::move(callback));
}

void unwatchWindowMotion()
{
    x11UnwatchWindowMotion(x11Display());
}

void observeForegroundChanges(std::function<void()> callback)
{
    observers().foregroundChanged = std::move(callback);
}

void unobserveForegroundChanges()
{
    observers().foregroundChanged = nullptr;
}

std::vector<DesktopWindow> attachCandidateWindows(uint32_t displayId)
{
    // No level shifts here: a panel that floats above the ordinary layer is
    // an ordinary managed window to the stacking list, so the same list is
    // the answer - as it is on Windows.
    return onScreenWindows(displayId);
}

std::string displayName(uint32_t displayId)
{
    for (const LinuxDisplay& display : connectedDisplays()) {
        if (display.id == displayId) {
            return display.name;
        }
    }
    return "Display";
}

std::optional<uint64_t> focusedAttachedWindow(int64_t applicationPid, const std::vector<uint64_t>& attached)
{
    // The shared rule decides; this layer only harvests the stacking order it
    // reasons over, the same division the Windows layer keeps.
    return resolveAttachedFocus(x11OrderedWindows(x11Display()), applicationPid, attached);
}

void raiseWindow(uint64_t identity, int64_t)
{
    x11ActivateWindow(x11Display(), identity);
}

void rememberApplicationWindow(void*)
{
    // Nothing addresses the window by native identity on Linux.
}

int64_t ownApplicationPid()
{
    return getpid();
}

int64_t foregroundApplicationPid()
{
    return x11ForegroundApplicationPid(x11Display());
}

std::vector<std::string> interfaceFontFiles()
{
    // The common distribution layouts, best-coverage faces first; the
    // application loads the first that exists and falls back to the built-in
    // font past the end.
    return {
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    };
}

std::vector<std::string> monospaceFontFiles()
{
    return {
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    };
}

float monospaceFontScale()
{
    return 1.0f;
}

bool captureExclusionDisabled()
{
    static const bool disabled = std::getenv("SIDESCOPES_NO_CAPTURE_EXCLUSION") != nullptr;
    return disabled;
}

bool captureVisibilityToggleSupported()
{
    return false;
}

void setCaptureVisibility(bool)
{
}

bool captureVisible()
{
    // Portal capture sees every window; nothing excludes this application.
    return true;
}

void observeSystemWake(std::function<void()> callback)
{
    observers().systemWake = std::move(callback);
}

void observeEscapeWithoutKeyWindow(std::function<void()> callback)
{
    // The gap cannot occur yet: no Linux surface takes keys without focusing
    // the application.
    observers().escapeWithoutKeyWindow = std::move(callback);
}

// sampleScreenColorAsync and captureDisplayImage - the off-stream screen reads
// - live in x11_screen_read.cpp beside their XGetImage machinery.

}  // namespace sidescopes
