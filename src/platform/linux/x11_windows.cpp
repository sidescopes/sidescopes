// The X11/EWMH window services behind the Linux desktop seam. Coordinates
// are root coordinates, which ARE the global desktop space: XWayland maps
// its X screen onto the desktop one to one.

#include "platform/linux/x11_windows.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

#include "platform/linux/x11_error_guard.h"

namespace sidescopes {
namespace {

/// The EWMH atoms this layer reads, interned once. Atom values belong to the
/// server rather than to a connection, so the watcher thread's own
/// connection sees the same numbers.
struct X11Atoms
{
    Atom clientListStacking = None;
    Atom activeWindow = None;
    Atom windowPid = None;
    Atom windowType = None;
    Atom windowTypeNormal = None;
    Atom windowState = None;
    Atom windowStateHidden = None;
    Atom windowName = None;
    Atom watchWake = None;
};

const X11Atoms& atoms(Display* display)
{
    static const X11Atoms interned = [display] {
        X11Atoms known;
        known.clientListStacking = XInternAtom(display, "_NET_CLIENT_LIST_STACKING", False);
        known.activeWindow = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
        known.windowPid = XInternAtom(display, "_NET_WM_PID", False);
        known.windowType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
        known.windowTypeNormal = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
        known.windowState = XInternAtom(display, "_NET_WM_STATE", False);
        known.windowStateHidden = XInternAtom(display, "_NET_WM_STATE_HIDDEN", False);
        known.windowName = XInternAtom(display, "_NET_WM_NAME", False);
        known.watchWake = XInternAtom(display, "_SIDESCOPES_WATCH_WAKE", False);

        return known;
    }();

    return interned;
}

/// A 32-bit property's values. Xlib hands CARDINAL, WINDOW and ATOM
/// properties back as an array of long - not of uint32_t - whatever the wire
/// format says, and the length must be read the same way. Empty when the
/// property is absent, of another type, or the window is gone.
std::vector<unsigned long> cardinalProperty(Display* display, ::Window window, Atom property, Atom type)
{
    constexpr long MaxItems = 1024;
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, property, 0, MaxItems, False, type, &actualType, &actualFormat, &count,
                           &remaining, &data) != Success) {
        return {};
    }
    std::vector<unsigned long> values;
    if (data != nullptr) {
        if (actualFormat == 32) {
            const auto* items = reinterpret_cast<const unsigned long*>(data);
            values.assign(items, items + count);
        }
        XFree(data);
    }

    return values;
}

/// A text property's bytes. _NET_WM_NAME is UTF-8 by specification; the
/// ICCCM WM_NAME fallback is whatever the client wrote and is passed through
/// unconverted, which is what every X tool does with it.
std::string textProperty(Display* display, ::Window window, Atom property)
{
    XTextProperty text{};
    if (XGetTextProperty(display, window, &text, property) == 0 || text.value == nullptr) {
        return {};
    }
    std::string value(reinterpret_cast<const char*>(text.value), text.nitems);
    XFree(text.value);

    return value;
}

int64_t windowPid(Display* display, ::Window window)
{
    const std::vector<unsigned long> pid = cardinalProperty(display, window, atoms(display).windowPid, XA_CARDINAL);

    return pid.empty() ? 0 : static_cast<int64_t>(pid.front());
}

bool windowMinimized(Display* display, ::Window window)
{
    const std::vector<unsigned long> state = cardinalProperty(display, window, atoms(display).windowState, XA_ATOM);

    return std::find(state.begin(), state.end(), atoms(display).windowStateHidden) != state.end();
}

/// The window's title, the live one: _NET_WM_NAME where the client sets it,
/// the ICCCM name otherwise.
std::string windowTitle(Display* display, ::Window window)
{
    std::string title = textProperty(display, window, atoms(display).windowName);
    if (title.empty()) {
        title = textProperty(display, window, XA_WM_NAME);
    }

    return title;
}

/// WM_CLASS's class name - the application's own, shared by every window it
/// opens, which is what the label and the auxiliary-window rule want.
std::string windowApplication(Display* display, ::Window window)
{
    XClassHint hint{};
    if (XGetClassHint(display, window, &hint) == 0) {
        return {};
    }
    std::string name = hint.res_class != nullptr ? hint.res_class : "";
    if (hint.res_name != nullptr) {
        XFree(hint.res_name);
    }
    if (hint.res_class != nullptr) {
        XFree(hint.res_class);
    }

    return name;
}

/// Whether the window is one the user works in. EWMH's type is the explicit
/// statement Windows has no equivalent of; a client old enough not to set it
/// is admitted on WM_CLASS, which every real application carries and the
/// scratch windows toolkits leave on the list do not.
bool windowTypeOrdinary(Display* display, ::Window window)
{
    const std::vector<unsigned long> types = cardinalProperty(display, window, atoms(display).windowType, XA_ATOM);
    if (types.empty()) {
        return !windowApplication(display, window).empty();
    }

    return std::find(types.begin(), types.end(), atoms(display).windowTypeNormal) != types.end();
}

/// The window's rectangle in global desktop points. The rectangle is the
/// CLIENT area, not the manager's frame: a region attaches to what the
/// application draws, and x11WindowGeometry must report the same rectangle
/// x11OnScreenWindows offered.
std::optional<WindowGeometry> rootRectangle(Display* display, ::Window window, const XWindowAttributes& attributes)
{
    int rootX = 0;
    int rootY = 0;
    ::Window child = 0;
    if (XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &rootX, &rootY, &child) == 0) {
        return std::nullopt;
    }
    WindowGeometry geometry;
    geometry.x = rootX;
    geometry.y = rootY;
    geometry.width = attributes.width;
    geometry.height = attributes.height;

    return geometry;
}

/// The managed windows, frontmost first: _NET_CLIENT_LIST_STACKING runs
/// bottom to top, and every contract here is front to back.
std::vector<::Window> stackedWindows(Display* display)
{
    const std::vector<unsigned long> stacking =
        cardinalProperty(display, DefaultRootWindow(display), atoms(display).clientListStacking, XA_WINDOW);
    std::vector<::Window> windows;
    windows.reserve(stacking.size());
    for (auto entry = stacking.rbegin(); entry != stacking.rend(); ++entry) {
        windows.push_back(static_cast<::Window>(*entry));
    }

    return windows;
}

/// One on-screen application window, or nothing when the window fails a
/// rule. The rules are the Windows layer's, said in EWMH: mapped, not this
/// process's, an ordinary type, and not minimized - on screen means on
/// screen, and a minimized window answers x11WindowGeometry instead.
std::optional<DesktopWindow> describeWindow(Display* display, ::Window window, int64_t ownPid)
{
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0 || attributes.map_state != IsViewable) {
        return std::nullopt;
    }
    const int64_t pid = windowPid(display, window);
    if (pid == ownPid || windowMinimized(display, window) || !windowTypeOrdinary(display, window)) {
        return std::nullopt;
    }
    const std::optional<WindowGeometry> rect = rootRectangle(display, window, attributes);
    if (!rect) {
        return std::nullopt;
    }
    DesktopWindow entry;
    entry.x = rect->x;
    entry.y = rect->y;
    entry.width = rect->width;
    entry.height = rect->height;
    entry.application = windowApplication(display, window);
    entry.windowIdentity = static_cast<uint64_t>(window);
    entry.ownerPid = pid;

    return entry;
}

/// Anything smaller than this is chrome rather than a window a region
/// attaches to - the Windows layer's own floor, kept identical.
constexpr double MinimumWindowSide = 64.0;

double overlapArea(const DesktopWindow& window, const DisplayGeometry& geometry)
{
    const double left = std::max(window.x, geometry.originX);
    const double top = std::max(window.y, geometry.originY);
    const double right = std::min(window.x + window.width, geometry.originX + geometry.widthPoints);
    const double bottom = std::min(window.y + window.height, geometry.originY + geometry.heightPoints);
    if (right <= left || bottom <= top) {
        return 0.0;
    }

    return (right - left) * (bottom - top);
}

/// Whether the window belongs to @p displayId: the display it overlaps most,
/// so a window straddling two is listed once - what MonitorFromWindow
/// decides on Windows.
bool windowOnDisplay(const DesktopWindow& window, const std::vector<LinuxDisplay>& displays, uint32_t displayId)
{
    if (window.width < MinimumWindowSide || window.height < MinimumWindowSide) {
        return false;
    }
    double best = 0.0;
    uint32_t bestId = 0;
    for (const LinuxDisplay& candidate : displays) {
        const double area = overlapArea(window, candidate.geometry);
        if (area > best) {
            best = area;
            bestId = candidate.id;
        }
    }

    return best > 0.0 && bestId == displayId;
}

/// The live motion watch. The watcher thread owns @c display for the whole
/// of its life; the caller's thread only ever addresses @c wake, and only
/// over its own connection.
struct MotionWatch
{
    Display* display = nullptr;
    ::Window target = 0;
    ::Window frame = 0;
    ::Window wake = 0;
    std::function<void(WindowMotionSignal)> callback;
    std::atomic<bool> stopping{false};
    std::thread thread;
};

MotionWatch& motionWatch()
{
    static MotionWatch watch;

    return watch;
}

/// The window manager's frame around @p window, or None where the window is
/// not reparented. A reparenting manager - every real one, XWayland's
/// included - moves the FRAME when the user drags the title strip, and the
/// client learns of that only through the ICCCM's synthetic ConfigureNotify;
/// watching both is what makes a move impossible to miss.
::Window frameWindow(Display* display, ::Window window)
{
    ::Window root = 0;
    ::Window parent = 0;
    ::Window* children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(display, window, &root, &parent, &children, &count) == 0) {
        return None;
    }
    if (children != nullptr) {
        XFree(children);
    }

    return parent == root ? None : parent;
}

/// The watcher's loop. It blocks in XNextEvent so a move reaches the
/// application the moment it happens rather than at the next frame, and it
/// leaves only on the wake message x11UnwatchWindowMotion sends.
///
/// DELIVERY IS OFF THE MAIN THREAD, which the desktop seam's header does not
/// promise. It is safe because of what the callback contract itself allows a
/// handler to do and what this application's handler does: App::
/// onWindowMotion records the moment and wakes the frame loop through
/// glfwPostEmptyEvent, which GLFW documents as callable from any thread, and
/// every decision the signal feeds is taken by the loop afterwards. There is
/// no X11 route to a main-thread queue for a foreign window's events - they
/// arrive on this connection or not at all - and the frame loop's own
/// per-frame geometry poll already raises the same signal, so the watch only
/// makes it earlier.
void runMotionWatch(MotionWatch* watch)
{
    for (;;) {
        XEvent event;
        XNextEvent(watch->display, &event);
        if (watch->stopping.load(std::memory_order_acquire)) {
            break;
        }
        // Only the target and its frame have StructureNotifyMask selected on
        // this connection, so any configure is the watched window's.
        if (event.type == ConfigureNotify && watch->callback) {
            watch->callback(WindowMotionSignal::Moved);
        }
    }
    XDestroyWindow(watch->display, watch->wake);
    XCloseDisplay(watch->display);
    watch->display = nullptr;
}

/// Binds the watch's own connection to the window. False when the window
/// died before the watch could take, which leaves nothing to stop.
bool bindMotionWatch(MotionWatch& watch, ::Window window)
{
    const X11ErrorGuard guard(watch.display);
    watch.target = window;
    watch.frame = frameWindow(watch.display, window);
    watch.wake = XCreateSimpleWindow(watch.display, DefaultRootWindow(watch.display), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(watch.display, watch.target, StructureNotifyMask);
    if (watch.frame != None) {
        XSelectInput(watch.display, watch.frame, StructureNotifyMask);
    }

    return !guard.failed();
}

/// Ends the watcher's blocking wait. A client message to the watcher's own
/// window, sent over the CALLER's connection: the empty event mask routes it
/// to the client that created the window - the watcher - and nothing here
/// touches the watcher's connection.
void wakeMotionWatch(Display* display, const MotionWatch& watch)
{
    if (display == nullptr || watch.wake == 0) {
        return;
    }
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = watch.wake;
    event.xclient.message_type = atoms(display).watchWake;
    event.xclient.format = 32;
    const X11ErrorGuard guard(display);
    XSendEvent(display, watch.wake, False, NoEventMask, &event);
    XFlush(display);
}

}  // namespace

std::vector<DesktopWindow> x11OnScreenWindows(Display* display, const std::vector<LinuxDisplay>& displays,
                                              uint32_t displayId, int64_t ownPid)
{
    std::vector<DesktopWindow> windows;
    if (display == nullptr) {
        return windows;
    }
    const X11ErrorGuard guard(display);
    for (const ::Window window : stackedWindows(display)) {
        std::optional<DesktopWindow> described = describeWindow(display, window, ownPid);
        if (described && windowOnDisplay(*described, displays, displayId)) {
            windows.push_back(std::move(*described));
        }
    }

    return windows;
}

std::optional<WindowGeometry> x11WindowGeometry(Display* display, uint64_t identity)
{
    if (display == nullptr || identity == 0) {
        return std::nullopt;
    }
    const auto window = static_cast<::Window>(identity);
    const X11ErrorGuard guard(display);
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0) {
        return std::nullopt;
    }
    std::optional<WindowGeometry> geometry = rootRectangle(display, window, attributes);
    if (!geometry) {
        return std::nullopt;
    }
    // Minimized is reported rather than refused: the contract asks the
    // coupling to hide the scopes, not to read the window as closed.
    geometry->minimized = windowMinimized(display, window) || attributes.map_state != IsViewable;
    geometry->title = windowTitle(display, window);
    if (guard.failed()) {
        // The window died mid-query. Every field above is then a guess, and
        // nothing is what the caller reads as the window having closed.
        return std::nullopt;
    }

    return geometry;
}

std::vector<OrderedWindow> x11OrderedWindows(Display* display)
{
    std::vector<OrderedWindow> windows;
    if (display == nullptr) {
        return windows;
    }
    const X11ErrorGuard guard(display);
    for (const ::Window window : stackedWindows(display)) {
        XWindowAttributes attributes{};
        if (XGetWindowAttributes(display, window, &attributes) == 0 || attributes.map_state != IsViewable ||
            windowMinimized(display, window)) {
            continue;
        }
        const std::optional<WindowGeometry> rect = rootRectangle(display, window, attributes);
        if (!rect) {
            continue;
        }
        windows.push_back(
            {static_cast<uint64_t>(window), windowPid(display, window), rect->x, rect->y, rect->width, rect->height});
    }

    return windows;
}

int64_t x11ForegroundApplicationPid(Display* display)
{
    if (display == nullptr) {
        return 0;
    }
    const X11ErrorGuard guard(display);
    const std::vector<unsigned long> active =
        cardinalProperty(display, DefaultRootWindow(display), atoms(display).activeWindow, XA_WINDOW);
    if (active.empty() || active.front() == None) {
        // No active window at all - a fresh session, or a manager that has
        // just unfocused one. Zero is the seam's word for it.
        return 0;
    }

    return windowPid(display, static_cast<::Window>(active.front()));
}

void x11ActivateWindow(Display* display, uint64_t identity)
{
    if (display == nullptr || identity == 0) {
        return;
    }
    // The window manager owns the stacking order, so a bare XRaiseWindow is
    // simply undone by it - EWMH's activation request is the way to ask, and
    // XWayland's manager is no different. Source indication 1 says an
    // application asked, which is what a pick is.
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = static_cast<::Window>(identity);
    event.xclient.message_type = atoms(display).activeWindow;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;
    event.xclient.data.l[1] = CurrentTime;
    event.xclient.data.l[2] = 0;
    const X11ErrorGuard guard(display);
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
    XFlush(display);
}

void x11WatchWindowMotion(Display* display, uint64_t identity, std::function<void(WindowMotionSignal)> callback)
{
    x11UnwatchWindowMotion(display);
    if (display == nullptr || identity == 0) {
        return;
    }
    MotionWatch& watch = motionWatch();
    // A connection of the watcher's own: it blocks in XNextEvent for as long
    // as the watch lasts, and Xlib serialises nothing between threads.
    watch.display = XOpenDisplay(nullptr);
    if (watch.display == nullptr) {
        return;
    }
    if (!bindMotionWatch(watch, static_cast<::Window>(identity))) {
        XCloseDisplay(watch.display);
        watch.display = nullptr;
        return;
    }
    // Written before the thread exists and cleared only after it is joined,
    // so the watcher never reads a callback the caller is changing.
    watch.callback = std::move(callback);
    watch.stopping.store(false, std::memory_order_release);
    watch.thread = std::thread(runMotionWatch, &watch);
}

void x11UnwatchWindowMotion(Display* display)
{
    MotionWatch& watch = motionWatch();
    if (!watch.thread.joinable()) {
        watch.callback = nullptr;
        return;
    }
    watch.stopping.store(true, std::memory_order_release);
    wakeMotionWatch(display, watch);
    watch.thread.join();
    watch.callback = nullptr;
    watch.target = 0;
    watch.frame = 0;
    watch.wake = 0;
}

}  // namespace sidescopes
