// Desktop services on Linux. Display geometry and live keyboard state come
// from X11/XRandR whenever a display is reachable - the X11 path also serves
// XWayland, so it covers every major desktop today. Where Wayland's isolation
// forbids a service outright (foreign window enumeration, off-window cursor
// reads, screen sampling), the seam reports the honest empty answer and the
// application's fallbacks carry it: those services return optionals or empty
// vectors by contract.

#include "platform/desktop.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>

#include "core/preferences.h"
#include "platform/linux/x11_displays.h"

namespace sidescopes {
namespace {

/// One X11 connection for the process, opened at first use. Null on a
/// pure-Wayland session without XWayland; every caller degrades on null.
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

std::vector<DesktopWindow> onScreenWindows(uint32_t)
{
    // Wayland gives applications no view of other applications' windows, so
    // window attach has no general Linux form; the portal's window source is
    // the planned replacement, with the compositor doing the choosing.
    return {};
}

std::optional<WindowGeometry> windowGeometry(uint64_t)
{
    return std::nullopt;
}

std::optional<DesktopPoint> globalCursorPosition()
{
    // XQueryPointer only tracks the cursor over X surfaces: exact on an X11
    // session, stale under XWayland while the cursor visits Wayland windows.
    // The capture stream's cursor metadata will replace this for the probes;
    // until then a stale read is worse than none.
    return std::nullopt;
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
    return std::nullopt;
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
    Display* display = x11Display();
    if (display == nullptr) {
        return state;
    }
    Window root = 0;
    Window child = 0;
    int rootX = 0;
    int rootY = 0;
    int childX = 0;
    int childY = 0;
    unsigned int mask = 0;
    if (XQueryPointer(display, DefaultRootWindow(display), &root, &child, &rootX, &rootY, &childX, &childY, &mask) ==
        True) {
        state.shift = (mask & ShiftMask) != 0;
        state.control = (mask & ControlMask) != 0;
        state.option = (mask & Mod1Mask) != 0;
        state.command = (mask & Mod4Mask) != 0;
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

void watchWindowMotion(uint64_t, int64_t, std::function<void(WindowMotionSignal)> callback)
{
    // Foreign window motion is unobservable on Wayland; the moved-from
    // callback is simply dropped, and the border - which does not exist here
    // yet either - would live on polled geometry instead.
    const std::function<void(WindowMotionSignal)> dropped = std::move(callback);
}

void unwatchWindowMotion()
{
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

std::optional<uint64_t> focusedAttachedWindow(int64_t, const std::vector<uint64_t>&)
{
    return std::nullopt;
}

void raiseWindow(uint64_t, int64_t)
{
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
    return 0;
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

void sampleScreenColorAsync(DesktopPoint, std::function<void(std::optional<FloatColor>)> callback)
{
    // No off-stream screen read exists under Wayland; the cursor readout
    // works from the capture frame alone. The synchronous empty answer is the
    // contract's way of saying so.
    const std::function<void(std::optional<FloatColor>)> reader = std::move(callback);
    if (reader) {
        reader(std::nullopt);
    }
}

std::optional<CapturedImage> captureDisplayImage(uint32_t)
{
    return std::nullopt;
}

}  // namespace sidescopes
