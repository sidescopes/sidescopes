"""The Linux system calls the scenario harness makes for itself.

The same three things quartz.py binds on macOS, bound the same way and under the
same names: the display layout a run must discover instead of assume, the
pointer and keyboard events that drive the application, and the per-process
counters a measurement reports. ctypes rather than a compiled helper, so the
harness still needs no build step of its own.

Two more live here that macOS never has to ask: which window to bring forward
before a shortcut is pressed - `open` does that there, and X has no notion of an
application at all - and which display system a launched application will end up
on, which decides whether it captures the screen itself or through the portal.

Five places where Linux is not macOS. They are stated here rather than
translated quietly, because each one would otherwise read as a difference in the
application:

COORDINATES ARE PIXELS. X11 has no point space of its own - the root window's
coordinates are backing pixels - so a display's `points` are its `pixels` and
`scale` is 1.0. A region asked for at 1600x1000 captured pixels therefore comes
out 1600x1000 wide, where a Retina Mac would have halved it.

THE COMMAND MODIFIER IS CONTROL. The application binds Ctrl+W and Ctrl+Q here,
not the Super key (`platformMinimizesWindowOnControlW` and
`platformQuitsOnControlQ` in src/platform/linux/desktop.cpp), so `command=True`
presses Control. Pressing Super would press a chord the application does not
have and the harness would report a shortcut that did nothing.

THERE IS NO PHYS_FOOTPRINT. `footprint_bytes` is resident plus what has been
swapped out, which is what the kernel charges the process; on a machine that has
not swapped it equals `resident_bytes`. It is NOT the macOS figure, and a
footprint from one system does not compare with a footprint from the other.

A WINDOW'S OWNER IS NOT ALWAYS ITS PROPERTY. `_NET_WM_PID` is set by the toolkit
on the managed window and by nothing else, so it cannot see the region border -
an override-redirect window on a SECOND X connection from the same process
(`overlayDisplay()` in src/platform/linux/x11_overlay.cpp). The border is
precisely what a region is confirmed by, so ownership is read from the
X-Resource extension, which knows which process opened each connection, and the
property is the fallback where that extension is missing.

A LAUNCHED APPLICATION INHERITS ITS DISPLAY SYSTEM. There is one window server
on a Mac; here the application takes whichever the environment names, and that
choice changes what it does - the X11 build captures the screen itself with
XShm, the Wayland one goes through the portal and PipeWire. So the environment a
measured launch gets is composed rather than passed on
(`application_environment`), and what it settles on is recorded with the run
(`session_facts`).

Whether a synthesised event arrives at all is a property of the SESSION rather
than of this code. An X session delivers them; a Wayland compositor owns the
pointer itself and what it does with an XTest request through Xwayland is its
own business, and where it discards them it does so silently. So the arrival is
MEASURED rather than assumed - `pointer_works()` moves the pointer and reads it
back, which is the same probe, for the same reason, as the Accessibility check
on macOS.
"""

import ctypes
import ctypes.util
import os
import threading
import time


def _library(name, soname):
    """One shared library, by whichever of its names this system carries."""
    for candidate in (ctypes.util.find_library(name), soname):
        if candidate:
            try:
                return ctypes.CDLL(candidate)
            except OSError:
                continue

    return None


_x11 = _library("X11", "libX11.so.6")
_xtest = _library("Xtst", "libXtst.so.6")
# Both optional, and the harness says what it lost rather than failing: without
# XRandR a run measures the whole X screen as one display, and without the
# X-Resource extension it can only see the windows a window manager labelled.
_xrandr = _library("Xrandr", "libXrandr.so.2")
_xres = _library("XRes", "libXRes.so.1")

if _x11 is None or _xtest is None:
    raise ImportError("the scenario harness needs libX11 and libXtst")

_Window = ctypes.c_ulong
_Atom = ctypes.c_ulong

_IS_VIEWABLE = 2
_BUTTON_LEFT = 1
_ANY_PROPERTY_TYPE = 0


class _XWindowAttributes(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_int), ("y", ctypes.c_int),
        ("width", ctypes.c_int), ("height", ctypes.c_int),
        ("border_width", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("visual", ctypes.c_void_p),
        ("root", _Window),
        ("window_class", ctypes.c_int),
        ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int),
        ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong),
        ("backing_pixel", ctypes.c_ulong),
        ("save_under", ctypes.c_int),
        ("colormap", ctypes.c_ulong),
        ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int),
        ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long),
        ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int),
        ("screen", ctypes.c_void_p),
    ]


class _XClientMessageEvent(ctypes.Structure):
    """A client message laid out in the space a whole XEvent occupies.

    XSendEvent takes the union, not this member of it, and Xlib pads that union
    to twenty-four longs - so the tail is carried here. A structure ending after
    the message's own fields would have the server reading past it.
    """

    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("window", _Window),
        ("message_type", _Atom),
        ("format", ctypes.c_int),
        ("data", ctypes.c_long * 5),
        ("tail", ctypes.c_long * 16),
    ]


_x11.XInitThreads.restype = ctypes.c_int
_x11.XOpenDisplay.restype = ctypes.c_void_p
_x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
_x11.XDefaultRootWindow.restype = _Window
_x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
_x11.XDefaultScreen.restype = ctypes.c_int
_x11.XDefaultScreen.argtypes = [ctypes.c_void_p]
_x11.XDisplayWidth.argtypes = [ctypes.c_void_p, ctypes.c_int]
_x11.XDisplayHeight.argtypes = [ctypes.c_void_p, ctypes.c_int]
_x11.XFlush.argtypes = [ctypes.c_void_p]
_x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
_x11.XFree.argtypes = [ctypes.c_void_p]
_x11.XInternAtom.restype = _Atom
_x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
_x11.XQueryTree.argtypes = [ctypes.c_void_p, _Window, ctypes.POINTER(_Window), ctypes.POINTER(_Window),
                            ctypes.POINTER(ctypes.POINTER(_Window)), ctypes.POINTER(ctypes.c_uint)]
_x11.XGetWindowAttributes.argtypes = [ctypes.c_void_p, _Window, ctypes.POINTER(_XWindowAttributes)]
_x11.XTranslateCoordinates.argtypes = [ctypes.c_void_p, _Window, _Window, ctypes.c_int, ctypes.c_int,
                                       ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                       ctypes.POINTER(_Window)]
_x11.XQueryPointer.argtypes = [ctypes.c_void_p, _Window, ctypes.POINTER(_Window), ctypes.POINTER(_Window),
                               ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                               ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                               ctypes.POINTER(ctypes.c_uint)]
_x11.XGetWindowProperty.argtypes = [ctypes.c_void_p, _Window, _Atom, ctypes.c_long, ctypes.c_long, ctypes.c_int,
                                    _Atom, ctypes.POINTER(_Atom), ctypes.POINTER(ctypes.c_int),
                                    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                                    ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
_x11.XStringToKeysym.restype = ctypes.c_ulong
_x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
_x11.XKeysymToKeycode.restype = ctypes.c_ubyte
_x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
_x11.XSendEvent.argtypes = [ctypes.c_void_p, _Window, ctypes.c_int, ctypes.c_long,
                            ctypes.POINTER(_XClientMessageEvent)]
_x11.XRaiseWindow.argtypes = [ctypes.c_void_p, _Window]
_x11.XSetInputFocus.argtypes = [ctypes.c_void_p, _Window, ctypes.c_int, ctypes.c_ulong]
_x11.XGetInputFocus.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Window), ctypes.POINTER(ctypes.c_int)]
_x11.XGetSelectionOwner.restype = _Window
_x11.XGetSelectionOwner.argtypes = [ctypes.c_void_p, _Atom]

_xtest.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
_xtest.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
_xtest.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]

_ERROR_HANDLER = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
_x11.XSetErrorHandler.restype = ctypes.c_void_p
_x11.XSetErrorHandler.argtypes = [_ERROR_HANDLER]


def _ignore_error(display, event):
    """Xlib's own handler EXITS THE PROCESS on a protocol error, and a window
    vanishing between being listed and being asked about is an ordinary event
    here - the application creates and destroys the border and the picker
    overlay while the harness is walking the tree. Every call that can fail is
    checked by its return value instead."""
    del display, event

    return 0


# Held at module scope because ctypes does not: a callback that is collected
# leaves the X library calling into freed memory.
_ERROR_SINK = _ERROR_HANDLER(_ignore_error)

_open_lock = threading.Lock()
_connection = None


def _display():
    """The harness's own X connection, opened once and shared.

    Opened on first use rather than at import, so that a run listing the
    catalogue - or any tool importing this module - does not need a display.
    Threads matter: the actions run on their own thread while the main thread
    samples, so XInitThreads has to come before the connection exists.
    """
    global _connection
    with _open_lock:
        if _connection is None:
            _x11.XInitThreads()
            _x11.XSetErrorHandler(_ERROR_SINK)
            opened = _x11.XOpenDisplay(None)
            if not opened:
                raise RuntimeError(f"cannot open the X display {os.environ.get('DISPLAY', '(DISPLAY unset)')}")
            _connection = opened

        return _connection


# --- The display layout -----------------------------------------------------


class _XRRScreenResources(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_ulong),
        ("configTimestamp", ctypes.c_ulong),
        ("ncrtc", ctypes.c_int),
        ("crtcs", ctypes.POINTER(ctypes.c_ulong)),
        ("noutput", ctypes.c_int),
        ("outputs", ctypes.POINTER(ctypes.c_ulong)),
        ("nmode", ctypes.c_int),
        ("modes", ctypes.c_void_p),
    ]


class _XRRCrtcInfo(ctypes.Structure):
    _fields_ = [
        ("timestamp", ctypes.c_ulong),
        ("x", ctypes.c_int), ("y", ctypes.c_int),
        ("width", ctypes.c_uint), ("height", ctypes.c_uint),
        ("mode", ctypes.c_ulong),
        ("rotation", ctypes.c_ushort),
        ("noutput", ctypes.c_int),
        ("outputs", ctypes.POINTER(ctypes.c_ulong)),
        ("rotations", ctypes.c_ushort),
        ("npossible", ctypes.c_int),
        ("possible", ctypes.POINTER(ctypes.c_ulong)),
    ]


if _xrandr is not None:
    _xrandr.XRRQueryExtension.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
    _xrandr.XRRGetScreenResourcesCurrent.restype = ctypes.POINTER(_XRRScreenResources)
    _xrandr.XRRGetScreenResourcesCurrent.argtypes = [ctypes.c_void_p, _Window]
    _xrandr.XRRFreeScreenResources.argtypes = [ctypes.POINTER(_XRRScreenResources)]
    _xrandr.XRRGetCrtcInfo.restype = ctypes.POINTER(_XRRCrtcInfo)
    _xrandr.XRRGetCrtcInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(_XRRScreenResources), ctypes.c_ulong]
    _xrandr.XRRFreeCrtcInfo.argtypes = [ctypes.POINTER(_XRRCrtcInfo)]
    _xrandr.XRRGetOutputPrimary.restype = ctypes.c_ulong
    _xrandr.XRRGetOutputPrimary.argtypes = [ctypes.c_void_p, _Window]


def _display_entry(identifier, x, y, width, height, main):
    """One display in the shape quartz.displays() returns.

    `points` and `pixels` are the same numbers, and `scale` is 1.0, because the
    X11 coordinates the pointer and the capture both work in ARE the backing
    pixels. Recorded in both places anyway so that a results file from either
    system is read the same way.
    """
    return {
        "id": int(identifier),
        "origin": [float(x), float(y)],
        "points": [float(width), float(height)],
        "pixels": [int(width), int(height)],
        "scale": 1.0,
        "main": bool(main),
    }


def _whole_screen():
    """The X screen as one display, for a server with no XRandR."""
    display = _display()
    screen = _x11.XDefaultScreen(display)

    return [_display_entry(0, 0, 0, _x11.XDisplayWidth(display, screen), _x11.XDisplayHeight(display, screen), True)]


def _randr_displays():
    """Every enabled CRTC, or None when the server cannot be asked."""
    display = _display()
    event_base = ctypes.c_int()
    error_base = ctypes.c_int()
    if not _xrandr.XRRQueryExtension(display, ctypes.byref(event_base), ctypes.byref(error_base)):
        return None
    root = _x11.XDefaultRootWindow(display)
    resources = _xrandr.XRRGetScreenResourcesCurrent(display, root)
    if not resources:
        return None
    primary = _xrandr.XRRGetOutputPrimary(display, root)
    found = []
    try:
        for index in range(resources.contents.ncrtc):
            identifier = resources.contents.crtcs[index]
            info = _xrandr.XRRGetCrtcInfo(display, resources, identifier)
            if not info:
                continue
            try:
                crtc = info.contents
                # A CRTC with no mode is an output that is connected and off.
                if crtc.mode == 0 or crtc.width == 0 or crtc.height == 0:
                    continue
                outputs = [crtc.outputs[slot] for slot in range(crtc.noutput)]
                found.append(_display_entry(identifier, crtc.x, crtc.y, crtc.width, crtc.height,
                                            primary != 0 and primary in outputs))
            finally:
                _xrandr.XRRFreeCrtcInfo(info)
    finally:
        _xrandr.XRRFreeScreenResources(resources)
    if not found:
        return None
    # A layout with no output marked primary still has to name a main display,
    # and the one at the origin is where a window manager puts things.
    if not any(entry["main"] for entry in found):
        at_origin = next((entry for entry in found if entry["origin"] == [0.0, 0.0]), found[0])
        at_origin["main"] = True

    return found


def displays():
    """Every active display, in the root window's coordinate space."""
    if _xrandr is not None:
        found = _randr_displays()
        if found:
            return found

    return _whole_screen()


def display_containing(point):
    """The display holding a root-window point, or None."""
    x, y = point
    for display in displays():
        left, top = display["origin"]
        width, height = display["points"]
        if left <= x < left + width and top <= y < top + height:
            return display

    return None


# --- Which process owns a window --------------------------------------------


class _XResClient(ctypes.Structure):
    _fields_ = [("resource_base", ctypes.c_ulong), ("resource_mask", ctypes.c_ulong)]


class _XResClientIdSpec(ctypes.Structure):
    _fields_ = [("client", ctypes.c_ulong), ("mask", ctypes.c_uint)]


class _XResClientIdValue(ctypes.Structure):
    _fields_ = [("spec", _XResClientIdSpec), ("length", ctypes.c_long), ("value", ctypes.c_void_p)]


_XRES_CLIENT_ID_PID_MASK = 2

if _xres is not None:
    _xres.XResQueryClients.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                                       ctypes.POINTER(ctypes.POINTER(_XResClient))]
    _xres.XResQueryClientIds.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.POINTER(_XResClientIdSpec),
                                         ctypes.POINTER(ctypes.c_long),
                                         ctypes.POINTER(ctypes.POINTER(_XResClientIdValue))]
    _xres.XResGetClientPid.restype = ctypes.c_int
    _xres.XResGetClientPid.argtypes = [ctypes.POINTER(_XResClientIdValue)]
    _xres.XResClientIdsDestroy.argtypes = [ctypes.c_long, ctypes.POINTER(_XResClientIdValue)]

# The connection table is asked for again this often at most. Refreshing it on
# every call would cost a round trip per sample in the middle of a flick, which
# is measured every four milliseconds; leaving it forever would miss the second
# connection the application opens the first time it shows a border.
_CLIENT_TABLE_SECONDS = 0.5

_client_table = ()
_client_table_taken = 0.0


def _query_clients():
    """Every X connection's resource range and the process that opened it."""
    display = _display()
    count = ctypes.c_int()
    clients = ctypes.POINTER(_XResClient)()
    if not _xres.XResQueryClients(display, ctypes.byref(count), ctypes.byref(clients)):
        return ()
    ranges = {}
    specs = (_XResClientIdSpec * max(count.value, 1))()
    for index in range(count.value):
        base = clients[index].resource_base
        ranges[base] = clients[index].resource_mask
        specs[index].client = base
        specs[index].mask = _XRES_CLIENT_ID_PID_MASK
    _x11.XFree(clients)
    if not ranges:
        return ()
    number = ctypes.c_long(0)
    values = ctypes.POINTER(_XResClientIdValue)()
    # Judged by what it produced, not by what it returned: this call reports
    # success as Success, which is zero, and failure as zero as well.
    _xres.XResQueryClientIds(display, count.value, specs, ctypes.byref(number), ctypes.byref(values))
    if not values or number.value <= 0:
        return ()
    found = []
    try:
        for index in range(number.value):
            owner = _xres.XResGetClientPid(ctypes.byref(values[index]))
            base = values[index].spec.client
            if owner > 0 and base in ranges:
                found.append((base, ranges[base], owner))
    finally:
        _xres.XResClientIdsDestroy(number, values)

    return tuple(found)


def _client_owner(window):
    """The process that opened the connection a window id came from, or None.

    None means the question could not be answered - the extension is absent, or
    the window belongs to a connection that has since closed - and the caller
    falls back to the property rather than concluding the window is somebody
    else's.
    """
    global _client_table, _client_table_taken
    if _xres is None:
        return None
    now = time.monotonic()
    if now - _client_table_taken > _CLIENT_TABLE_SECONDS:
        _client_table = _query_clients()
        _client_table_taken = now
    for base, mask, owner in _client_table:
        if (window & ~mask) == base:
            return owner

    return None


def _window_property(window, name, count):
    """One property of one window, as (format, values), or (0, ()).

    Two shapes because Xlib returns two: a 32-bit property arrives as an array
    of C longs whatever the wire format says it is, and anything narrower
    arrives as bytes. The format comes back beside the values so that a caller
    asking for numbers can refuse a property that turned out to be text.

    `count` is in 32-bit units, which is what XGetWindowProperty counts in, so a
    text property reads up to four times that many bytes.
    """
    display = _display()
    atom = _x11.XInternAtom(display, name, True)
    if atom == 0:
        return (0, ())
    kind = _Atom()
    format_bits = ctypes.c_int()
    items = ctypes.c_ulong()
    remaining = ctypes.c_ulong()
    data = ctypes.POINTER(ctypes.c_ubyte)()
    if _x11.XGetWindowProperty(display, window, atom, 0, count, False, _ANY_PROPERTY_TYPE, ctypes.byref(kind),
                               ctypes.byref(format_bits), ctypes.byref(items), ctypes.byref(remaining),
                               ctypes.byref(data)) != 0:
        return (0, ())
    values = ()
    if data and format_bits.value == 32:
        listing = ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))
        values = tuple(int(listing[index]) for index in range(items.value))
    elif data and format_bits.value == 8:
        values = bytes(bytearray(data[index] for index in range(items.value)))
    if data:
        _x11.XFree(data)

    return (format_bits.value, values)


def _cardinals(window, name, count=1):
    """A property's 32-bit values; empty where it is missing or is not numbers."""
    format_bits, values = _window_property(window, name, count)

    return values if format_bits == 32 else ()


def _text_property(window, name):
    """A property's text; empty where it is missing or is not text."""
    format_bits, values = _window_property(window, name, 256)

    return values.decode("utf-8", "replace") if format_bits == 8 else ""


def _property_owner(window):
    """The `_NET_WM_PID` a toolkit put on a managed window, or None."""
    owner = _cardinals(window, b"_NET_WM_PID")

    return owner[0] if owner else None


def _owned_by(window, pid):
    return (_client_owner(window) or _property_owner(window)) == pid


def _children_of(window):
    """A window's children, innermost last, or an empty tuple."""
    display = _display()
    root = _Window()
    parent = _Window()
    children = ctypes.POINTER(_Window)()
    count = ctypes.c_uint()
    if not _x11.XQueryTree(display, window, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(children),
                           ctypes.byref(count)):
        return ()
    found = tuple(children[index] for index in range(count.value))
    if children:
        _x11.XFree(children)

    return found


def _client_list():
    """The window manager's `_NET_CLIENT_LIST`, which names managed windows.

    Read because a managed window is reparented into the window manager's frame
    and so is no longer a child of the root; this finds it without walking into
    every frame on the desktop.
    """
    return _cardinals(_x11.XDefaultRootWindow(_display()), b"_NET_CLIENT_LIST", 4096)


def _window_attributes(window):
    """A window's attributes, or None where it has gone since it was listed."""
    attributes = _XWindowAttributes()
    if not _x11.XGetWindowAttributes(_display(), window, ctypes.byref(attributes)):
        return None

    return attributes


def _window_rect(window):
    """A viewable window's frame in root coordinates, or None.

    Root coordinates rather than the window's own, because a managed window
    sits inside its window manager's frame and reports its position relative to
    that. Anything reading a window's own x and y measures the decoration.
    """
    display = _display()
    attributes = _window_attributes(window)
    if attributes is None or attributes.map_state != _IS_VIEWABLE:
        return None
    x = ctypes.c_int()
    y = ctypes.c_int()
    child = _Window()
    if not _x11.XTranslateCoordinates(display, window, _x11.XDefaultRootWindow(display), 0, 0, ctypes.byref(x),
                                      ctypes.byref(y), ctypes.byref(child)):
        return None

    return (float(x.value), float(y.value), float(attributes.width), float(attributes.height))


def windows_owned_by(pid):
    """The on-screen windows a process owns, as (x, y, width, height) pixels.

    This is how the harness confirms the application really took a region: the
    region border is a window of its own, at the region's geometry, in every
    version. Nothing in the application's own output says as much - a region
    over content that does not change runs no analysis at all, which is correct
    behaviour and useless as a signal.

    Two lists are searched rather than the whole tree, and the difference
    matters: the application's own window is managed, so it is in
    `_NET_CLIENT_LIST` and NOT a child of the root, while the border and the
    picker overlay are override-redirect, so they are children of the root and
    in no list a window manager keeps.
    """
    wanted = int(pid)
    seen = set()
    found = []
    for window in _children_of(_x11.XDefaultRootWindow(_display())) + _client_list():
        if window in seen:
            continue
        seen.add(window)
        if not _owned_by(window, wanted):
            continue
        frame = _window_rect(window)
        if frame is not None:
            found.append(frame)

    return found


# --- Bringing a window forward ----------------------------------------------

_CLIENT_MESSAGE = 33
_SUBSTRUCTURE_NOTIFY_MASK = 1 << 19
_SUBSTRUCTURE_REDIRECT_MASK = 1 << 20
_SOURCE_APPLICATION = 1
_REVERT_TO_PARENT = 2
_CURRENT_TIME = 0

# How long a window manager is given to act on an activation request. It is a
# round trip through another process rather than a call, so it is not
# immediate; a quarter of a second is far longer than a manager takes and still
# under the pause the caller waits afterwards.
_ACTIVATION_SECONDS = 0.25


def _top_level_window_of(pid):
    """The process's own window - the one that can hold the keyboard.

    Deliberately not any window the process owns: the region border and the
    picker overlay are override-redirect, so activating one would name a window
    no manager manages and the keyboard would stay wherever it already was.

    Where a manager is running its own list answers this. Where none is,
    nothing keeps such a list, so the root's children are read directly and the
    override-redirect ones are dropped by their attributes instead.
    """
    listed = _client_list()
    candidates = listed or _children_of(_x11.XDefaultRootWindow(_display()))
    for window in candidates:
        attributes = _window_attributes(window)
        if attributes is None or attributes.map_state != _IS_VIEWABLE:
            continue
        if not listed and attributes.override_redirect:
            continue
        if _owned_by(window, pid):
            return window

    return None


def _request_activation(window):
    """EWMH's "activate this window", which is how a manager is asked.

    A bare XRaiseWindow is simply undone by the manager, which owns the stacking
    order - this is the same request the application itself sends to raise a
    window it was attached to (`x11ActivateWindow` in
    src/platform/linux/x11_windows.cpp). Source indication 1 says an application
    asked, which is what the harness is standing in for.
    """
    display = _display()
    event = _XClientMessageEvent()
    event.type = _CLIENT_MESSAGE
    event.window = window
    event.message_type = _x11.XInternAtom(display, b"_NET_ACTIVE_WINDOW", False)
    event.format = 32
    event.data[0] = _SOURCE_APPLICATION
    event.data[1] = _CURRENT_TIME
    _x11.XSendEvent(display, _x11.XDefaultRootWindow(display), False,
                    _SUBSTRUCTURE_NOTIFY_MASK | _SUBSTRUCTURE_REDIRECT_MASK, ctypes.byref(event))
    _x11.XFlush(display)


def _focused_window():
    """The window the keyboard is going to."""
    window = _Window()
    revert = ctypes.c_int()
    _x11.XGetInputFocus(_display(), ctypes.byref(window), ctypes.byref(revert))

    return window.value


def _window_manager_window():
    """The window a running manager publishes to prove it is there, or zero.

    The one test for a manager being present, and the one place its name is
    read from, so that a manager which publishes the window but no name is not
    mistaken for no manager at all.
    """
    found = _cardinals(_x11.XDefaultRootWindow(_display()), b"_NET_SUPPORTING_WM_CHECK")

    return found[0] if found else 0


def activate_window(pid):
    """Bring a process's window forward and give it the keyboard.

    Asked of a WINDOW rather than of a program, because X has no notion of an
    application: there is nothing here that corresponds to `open`, which
    activates a bundle on macOS. Where a manager is running it is asked through
    EWMH and ITS ANSWER IS READ BACK rather than assumed - a request that was
    declined would leave the harness pressing plain letters at whatever held the
    keyboard before, which is exactly how shortcuts were being lost on the other
    system.

    @return Whether the window ended up with the keyboard.
    """
    window = _top_level_window_of(pid)
    if window is None:
        return False
    if not _window_manager_window():
        # Nothing owns the stacking order or the focus, so both are set
        # directly. A managed session must not take this path - a manager would
        # undo the raise and take the focus back.
        display = _display()
        _x11.XRaiseWindow(display, window)
        _x11.XSetInputFocus(display, window, _REVERT_TO_PARENT, _CURRENT_TIME)
        _x11.XFlush(display)

        return _focused_window() == window
    _request_activation(window)
    root = _x11.XDefaultRootWindow(_display())
    deadline = time.monotonic() + _ACTIVATION_SECONDS
    while time.monotonic() < deadline:
        active = _cardinals(root, b"_NET_ACTIVE_WINDOW")
        if active and active[0] == window:
            return True
        time.sleep(0.02)

    return False


# --- The pointer and the keyboard -------------------------------------------

# Keysym names for the keys the harness presses, where they are not the name
# itself. The application binds plain letters, so a letter is its own keysym
# name and only the four named keys need spelling out.
#
# Unlike the macOS module these are not positions on the keyboard: X resolves a
# keysym to whichever key carries it under the CURRENT layout, so the harness
# presses the key that types the letter rather than the key in that place.
_KEY_NAMES = {
    "escape": "Escape",
    "space": "space",
    "return": "Return",
    "tab": "Tab",
}


def _keycode(name):
    keysym = _x11.XStringToKeysym(_KEY_NAMES.get(name, name).encode())
    code = _x11.XKeysymToKeycode(_display(), keysym) if keysym else 0
    if code == 0:
        raise KeyError(f"no key code for {name!r} on this keyboard layout")

    return code


def pointer_position():
    display = _display()
    root = _Window()
    child = _Window()
    root_x = ctypes.c_int()
    root_y = ctypes.c_int()
    window_x = ctypes.c_int()
    window_y = ctypes.c_int()
    modifiers = ctypes.c_uint()
    _x11.XQueryPointer(display, _x11.XDefaultRootWindow(display), ctypes.byref(root), ctypes.byref(child),
                       ctypes.byref(root_x), ctypes.byref(root_y), ctypes.byref(window_x), ctypes.byref(window_y),
                       ctypes.byref(modifiers))

    return (float(root_x.value), float(root_y.value))


def _post_motion(point):
    display = _display()
    _xtest.XTestFakeMotionEvent(display, _x11.XDefaultScreen(display), int(round(point[0])), int(round(point[1])), 0)
    _x11.XFlush(display)


def _post_button(point, pressed):
    """A button event at a place.

    X carries no location on a button event - it takes the pointer's - so the
    move is part of the press rather than a separate step, which is what the
    Quartz events this mirrors do in one call.
    """
    display = _display()
    _post_motion(point)
    _xtest.XTestFakeButtonEvent(display, _BUTTON_LEFT, pressed, 0)
    _x11.XFlush(display)


def move_pointer(point):
    _post_motion(point)


def click(point, settle=0.08):
    _post_motion(point)
    time.sleep(settle)
    _post_button(point, True)
    time.sleep(settle)
    _post_button(point, False)


def press_mouse(point):
    _post_button(point, True)


def drag_mouse(point):
    _post_motion(point)


def release_mouse(point):
    _post_button(point, False)


def drag(start, end, steps=30, step_seconds=0.016, settle=0.12):
    """A press, a straight sweep, and a release, at roughly a frame per step."""
    _post_motion(start)
    time.sleep(settle)
    _post_button(start, True)
    time.sleep(settle)
    for step in range(1, steps + 1):
        fraction = step / steps
        _post_motion((start[0] + ((end[0] - start[0]) * fraction),
                      start[1] + ((end[1] - start[1]) * fraction)))
        time.sleep(step_seconds)
    time.sleep(settle)
    _post_button(end, False)


def press_key(name, command=False, shift=False):
    """Press and release one key, optionally under the modifiers.

    `command` is CONTROL here. The application's dismissal and quit chords are
    Ctrl+W and Ctrl+Q on this system, so the caller that means "the platform's
    application modifier" gets the one this platform's build actually binds.
    """
    display = _display()
    code = _keycode(name.lower())
    modifiers = [_keycode(held) for held in (("Control_L",) if command else ()) + (("Shift_L",) if shift else ())]
    for modifier in modifiers:
        _xtest.XTestFakeKeyEvent(display, modifier, True, 0)
    _x11.XFlush(display)
    for pressed in (True, False):
        _xtest.XTestFakeKeyEvent(display, code, pressed, 0)
        _x11.XFlush(display)
        time.sleep(0.05)
    for modifier in reversed(modifiers):
        _xtest.XTestFakeKeyEvent(display, modifier, False, 0)
    _x11.XFlush(display)


# What to do when pointer_works() says no. Kept beside the reason it fails, so
# that the caller which reports it stays free of any one system's vocabulary.
POINTER_HELP = ("Run this on an X session rather than a Wayland one: a Wayland compositor owns the pointer, and "
                "where it will not be driven it says nothing.")


def pointer_works():
    """Whether synthesised pointer events actually reach the X server.

    Where they are discarded they are discarded quietly, and a whole run would
    then measure an application nobody touched. Moves the pointer a little and
    puts it back.

    The step is taken towards the middle of the screen rather than always down
    and to the right: X clamps the pointer to the screen, so a probe aimed off
    the edge would land short and read as a failure that is really a corner.
    """
    before = pointer_position()
    display = _display()
    screen = _x11.XDefaultScreen(display)
    room = (_x11.XDisplayWidth(display, screen), _x11.XDisplayHeight(display, screen))
    step = (20.0 if before[0] + 20.0 < room[0] else -20.0, 20.0 if before[1] + 20.0 < room[1] else -20.0)
    target = (before[0] + step[0], before[1] + step[1])
    move_pointer(target)
    time.sleep(0.25)
    after = pointer_position()
    move_pointer(before)

    return abs(after[0] - target[0]) < 2.0 and abs(after[1] - target[1]) < 2.0


# --- Per-process counters ---------------------------------------------------

# The processor-time fields in /proc are counted in clock ticks, which is a
# hundred a second on every Linux this will run on but is asked for rather than
# assumed - it is a build-time constant of the kernel, not of the architecture.
_TICK_NANOSECONDS = 1e9 / os.sysconf("SC_CLK_TCK")

_KILOBYTE = 1024

# A process that has exited but not been reaped still has a /proc entry, and
# reading one as a live process would leave a caller waiting for a departure
# that already happened.
_GONE_STATES = ("Z", "X", "x")


class ProcessSample:
    """One reading of a process: cumulative processor time and memory.

    `cpu_nanoseconds` is user plus system time, so a difference of two samples
    over a wall-clock interval is the CORES of one core the process cost. Never
    report it as a percentage: a percentage is per core in one tool and per
    machine in another, and the two disagree by the core count.

    It is named in nanoseconds and COUNTED IN CLOCK TICKS, a hundredth of a
    second apiece, which is worth knowing before reading a small number here: a
    process costing a thousandth of a core reads as zero over a ten-second
    window about as often as it reads as one tick. That is the floor of what
    /proc reports and not a process that did nothing. It is immaterial at the
    tenth-of-a-core scale a measured application sits at, and it is why the
    content window's own cost can come out as a flat zero.

    `footprint_bytes` is resident size plus what has been swapped out - the
    memory the kernel charges this process for. Linux has no `phys_footprint`,
    so this is the nearest honest figure rather than the same one, and on a
    machine that has not swapped it equals `resident_bytes` exactly. A footprint
    measured here does not compare with one measured on macOS.
    """

    def __init__(self, cpu_nanoseconds, footprint_bytes, resident_bytes):
        self.cpu_nanoseconds = cpu_nanoseconds
        self.footprint_bytes = footprint_bytes
        self.resident_bytes = resident_bytes


def _processor_ticks(pid):
    """User plus system ticks, and the process's state letter."""
    with open(f"/proc/{pid}/stat", "rb") as handle:
        line = handle.read()
    # The fields are counted from the LAST close parenthesis. A process name
    # sits in parentheses in the middle of the line and may itself contain
    # spaces and brackets, so splitting the whole line shifts every field after
    # it - silently, and only for the processes unlucky enough to be named that
    # way.
    fields = line[line.rindex(b")") + 1:].split()

    return (int(fields[11]) + int(fields[12]), fields[0].decode())


def _memory_bytes(pid):
    """Resident and swapped-out bytes, from the kernel's own summary."""
    resident = 0
    swapped = 0
    with open(f"/proc/{pid}/status", "rb") as handle:
        for line in handle:
            if line.startswith(b"VmRSS:"):
                resident = int(line.split()[1]) * _KILOBYTE
            elif line.startswith(b"VmSwap:"):
                swapped = int(line.split()[1]) * _KILOBYTE

    return (resident, swapped)


def process_sample(pid):
    """Read a process's counters, or None if it has gone."""
    try:
        ticks, state = _processor_ticks(pid)
        if state in _GONE_STATES:
            return None
        resident, swapped = _memory_bytes(pid)
    except (OSError, ValueError, IndexError):
        return None

    return ProcessSample(ticks * _TICK_NANOSECONDS, resident + swapped, resident)


# --- The session a measured application is launched into --------------------


def application_environment(base):
    """The environment a measured launch gets, with the display system settled.

    One rule, and it decides which half of the Linux port runs.
    `isX11Session()` in src/platform/linux/linux_session.cpp reads
    WAYLAND_DISPLAY FIRST, so a variable inherited from the desktop session
    sends the application to the portal even when it is being launched onto an X
    server. That X server is the one this module drives and reads windows from,
    so the application has to be on it: left in place, the variable would have a
    run driving one screen and measuring an application looking at another.
    """
    prepared = dict(base)
    if prepared.get("DISPLAY"):
        prepared.pop("WAYLAND_DISPLAY", None)

    return prepared


def _window_manager_name():
    """What the running window manager calls itself, or an empty string.

    Worth recording because it is not decoration: the manager owns the stacking
    order and the focus, and it is the other party in every drag the harness
    performs.
    """
    check = _window_manager_window()

    return _text_property(check, b"_NET_WM_NAME") if check else ""


def _compositing():
    """Whether a compositing manager owns the screen.

    It puts a copy of every window between what the application draws and what
    the panel shows, which is a cost the application does not pay and cannot
    see.
    """
    display = _display()
    selection = _x11.XInternAtom(display, f"_NET_WM_CM_S{_x11.XDefaultScreen(display)}".encode(), True)

    return selection != 0 and _x11.XGetSelectionOwner(display, selection) != 0


def session_facts():
    """Which display system the application will use, and what is in front of it.

    Recorded with every run because it changes WHAT IS BEING MEASURED rather
    than only how fast it is: on an X session the application captures the
    screen itself with XShm, and on a Wayland one through the portal and
    PipeWire, which is a different pipeline at a different cost.

    `session_type` is what the login session declares itself to be and
    `x11_session` is what the application's own test will decide from the
    environment it is handed. The two disagreeing is not a fault - it is a
    nested X server inside a Wayland login, which is how this harness is run on
    such a desktop - but a reader has to be able to see which of them the
    numbers belong to.
    """
    prepared = application_environment(os.environ)
    x11 = bool(prepared.get("DISPLAY")) and not prepared.get("WAYLAND_DISPLAY")

    return {
        "session_type": os.environ.get("XDG_SESSION_TYPE", ""),
        "display": prepared.get("DISPLAY", ""),
        "x11_session": x11,
        "capture": "xshm" if x11 else "portal",
        "window_manager": _window_manager_name(),
        "compositing": _compositing(),
    }
