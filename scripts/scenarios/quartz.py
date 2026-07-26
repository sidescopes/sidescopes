"""The macOS system calls the scenario harness makes for itself.

Three things live here, all of them thin bindings rather than logic: the display
layout a run must discover instead of assume, the pointer and keyboard events
that drive the application, and the per-process counters a measurement reports.

They are bound with ctypes rather than a compiled helper so that the harness
needs no build step of its own. A Windows harness would replace this module
wholesale - EnumDisplayMonitors, SendInput, GetProcessTimes and
GetProcessMemoryInfo cover the same ground - and nothing outside it is
platform-specific.

Posting events requires the ACCESSIBILITY permission for whichever application
runs this script (System Settings > Privacy & Security > Accessibility). Without
it the calls succeed silently and nothing moves, so callers should check
`pointer_works()` before trusting a run.
"""

import ctypes
import ctypes.util
import struct
import time

_APPLICATION_SERVICES = "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices"
_CORE_FOUNDATION = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"

_cg = ctypes.CDLL(_APPLICATION_SERVICES)
_cf = ctypes.CDLL(_CORE_FOUNDATION)
_libc = ctypes.CDLL(ctypes.util.find_library("c"))


class CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


class CGSize(ctypes.Structure):
    _fields_ = [("width", ctypes.c_double), ("height", ctypes.c_double)]


class CGRect(ctypes.Structure):
    _fields_ = [("origin", CGPoint), ("size", CGSize)]


_cf.CFRelease.argtypes = [ctypes.c_void_p]

_cg.CGDisplayBounds.restype = CGRect
_cg.CGDisplayBounds.argtypes = [ctypes.c_uint32]
_cg.CGGetActiveDisplayList.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                                       ctypes.POINTER(ctypes.c_uint32)]
_cg.CGDisplayIsMain.argtypes = [ctypes.c_uint32]
_cg.CGDisplayCopyDisplayMode.restype = ctypes.c_void_p
_cg.CGDisplayCopyDisplayMode.argtypes = [ctypes.c_uint32]
_cg.CGDisplayModeGetPixelWidth.argtypes = [ctypes.c_void_p]
_cg.CGDisplayModeGetPixelHeight.argtypes = [ctypes.c_void_p]
_cg.CGDisplayModeRelease.argtypes = [ctypes.c_void_p]

_cg.CGEventCreate.restype = ctypes.c_void_p
_cg.CGEventCreate.argtypes = [ctypes.c_void_p]
_cg.CGEventGetLocation.restype = CGPoint
_cg.CGEventGetLocation.argtypes = [ctypes.c_void_p]
_cg.CGEventCreateMouseEvent.restype = ctypes.c_void_p
_cg.CGEventCreateMouseEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint32, CGPoint, ctypes.c_uint32]
_cg.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
_cg.CGEventCreateKeyboardEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool]
_cg.CGEventSetFlags.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
_cg.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]

_HID_EVENT_TAP = 0
_MOUSE_MOVED = 5
_LEFT_DOWN = 1
_LEFT_UP = 2
_LEFT_DRAGGED = 6
_BUTTON_LEFT = 0

_FLAG_SHIFT = 0x00020000
_FLAG_COMMAND = 0x00100000

# Virtual key codes for the keys the harness presses. The application binds
# plain letters, so only letters and Escape are needed; the codes are positions
# on the keyboard, not characters, and are the same on every layout.
_KEY_CODES = {
    "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9,
    "b": 11, "q": 12, "w": 13, "e": 14, "r": 15, "y": 16, "t": 17, "o": 31, "u": 32,
    "i": 34, "p": 35, "l": 37, "j": 38, "k": 40, "n": 45, "m": 46,
    "escape": 53, "space": 49, "return": 36, "tab": 48,
}


# --- The display layout -----------------------------------------------------


def displays():
    """Every active display, in the global point space the pointer uses.

    `scale` is the backing pixels per point, so a run on a Retina panel and a
    run on a 1x monitor are visibly different conditions rather than two
    numbers that look alike.
    """
    identifiers = (ctypes.c_uint32 * 16)()
    count = ctypes.c_uint32()
    _cg.CGGetActiveDisplayList(16, identifiers, ctypes.byref(count))
    found = []
    for index in range(count.value):
        identifier = identifiers[index]
        bounds = _cg.CGDisplayBounds(identifier)
        mode = _cg.CGDisplayCopyDisplayMode(identifier)
        pixel_width = _cg.CGDisplayModeGetPixelWidth(mode) if mode else 0
        pixel_height = _cg.CGDisplayModeGetPixelHeight(mode) if mode else 0
        if mode:
            _cg.CGDisplayModeRelease(mode)
        found.append({
            "id": int(identifier),
            "origin": [bounds.origin.x, bounds.origin.y],
            "points": [bounds.size.width, bounds.size.height],
            "pixels": [pixel_width, pixel_height],
            "scale": round(pixel_width / bounds.size.width, 3) if bounds.size.width else 0.0,
            "main": bool(_cg.CGDisplayIsMain(identifier)),
        })

    return found


_cg.CGWindowListCopyWindowInfo.restype = ctypes.c_void_p
_cg.CGWindowListCopyWindowInfo.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
_cg.CGRectMakeWithDictionaryRepresentation.restype = ctypes.c_bool
_cg.CGRectMakeWithDictionaryRepresentation.argtypes = [ctypes.c_void_p, ctypes.POINTER(CGRect)]
_cf.CFStringCreateWithCString.restype = ctypes.c_void_p
_cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
_cf.CFArrayGetCount.restype = ctypes.c_long
_cf.CFArrayGetCount.argtypes = [ctypes.c_void_p]
_cf.CFArrayGetValueAtIndex.restype = ctypes.c_void_p
_cf.CFArrayGetValueAtIndex.argtypes = [ctypes.c_void_p, ctypes.c_long]
_cf.CFDictionaryGetValue.restype = ctypes.c_void_p
_cf.CFDictionaryGetValue.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_cf.CFNumberGetValue.restype = ctypes.c_bool
_cf.CFNumberGetValue.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]

_UTF8 = 0x08000100
_NUMBER_SINT64 = 4
_ON_SCREEN_ONLY = 1


def _cfstring(text):
    return _cf.CFStringCreateWithCString(None, text.encode(), _UTF8)


def windows_owned_by(pid):
    """The on-screen windows a process owns, as (x, y, width, height) points.

    This is how the harness confirms the application really took a region: the
    region border is a window of its own, at the region's geometry, in every
    version. Nothing in the application's own output says as much - a region
    over content that does not change runs no analysis at all, which is correct
    behaviour and useless as a signal.
    """
    listing = _cg.CGWindowListCopyWindowInfo(_ON_SCREEN_ONLY, 0)
    if not listing:
        return []
    owner_key = _cfstring("kCGWindowOwnerPID")
    bounds_key = _cfstring("kCGWindowBounds")
    found = []
    try:
        for index in range(_cf.CFArrayGetCount(listing)):
            entry = _cf.CFArrayGetValueAtIndex(listing, index)
            owner = _cf.CFDictionaryGetValue(entry, owner_key)
            value = ctypes.c_int64()
            if not owner or not _cf.CFNumberGetValue(owner, _NUMBER_SINT64, ctypes.byref(value)):
                continue
            if value.value != int(pid):
                continue
            bounds = _cf.CFDictionaryGetValue(entry, bounds_key)
            rect = CGRect()
            if bounds and _cg.CGRectMakeWithDictionaryRepresentation(bounds, ctypes.byref(rect)):
                found.append((rect.origin.x, rect.origin.y, rect.size.width, rect.size.height))
    finally:
        for reference in (listing, owner_key, bounds_key):
            _cf.CFRelease(reference)

    return found


def display_containing(point):
    """The display holding a global point, or None."""
    x, y = point
    for display in displays():
        left, top = display["origin"]
        width, height = display["points"]
        if left <= x < left + width and top <= y < top + height:
            return display

    return None


# --- The pointer and the keyboard -------------------------------------------


def pointer_position():
    event = _cg.CGEventCreate(None)
    location = _cg.CGEventGetLocation(event)
    _cf.CFRelease(event)

    return (location.x, location.y)


def _post_mouse(kind, point):
    event = _cg.CGEventCreateMouseEvent(None, kind, CGPoint(point[0], point[1]), _BUTTON_LEFT)
    _cg.CGEventPost(_HID_EVENT_TAP, event)
    _cf.CFRelease(event)


def move_pointer(point):
    _post_mouse(_MOUSE_MOVED, point)


def click(point, settle=0.08):
    _post_mouse(_MOUSE_MOVED, point)
    time.sleep(settle)
    _post_mouse(_LEFT_DOWN, point)
    time.sleep(settle)
    _post_mouse(_LEFT_UP, point)


def press_mouse(point):
    _post_mouse(_LEFT_DOWN, point)


def drag_mouse(point):
    _post_mouse(_LEFT_DRAGGED, point)


def release_mouse(point):
    _post_mouse(_LEFT_UP, point)


def drag(start, end, steps=30, step_seconds=0.016, settle=0.12):
    """A press, a straight sweep, and a release, at roughly a frame per step."""
    _post_mouse(_MOUSE_MOVED, start)
    time.sleep(settle)
    _post_mouse(_LEFT_DOWN, start)
    time.sleep(settle)
    for step in range(1, steps + 1):
        fraction = step / steps
        _post_mouse(_LEFT_DRAGGED, (start[0] + ((end[0] - start[0]) * fraction),
                                    start[1] + ((end[1] - start[1]) * fraction)))
        time.sleep(step_seconds)
    time.sleep(settle)
    _post_mouse(_LEFT_UP, end)


def press_key(name, command=False, shift=False):
    code = _KEY_CODES.get(name.lower())
    if code is None:
        raise KeyError(f"no virtual key code for {name!r}")
    flags = (_FLAG_COMMAND if command else 0) | (_FLAG_SHIFT if shift else 0)
    for pressed in (True, False):
        event = _cg.CGEventCreateKeyboardEvent(None, code, pressed)
        if flags:
            _cg.CGEventSetFlags(event, flags)
        _cg.CGEventPost(_HID_EVENT_TAP, event)
        _cf.CFRelease(event)
        time.sleep(0.05)


def pointer_works():
    """Whether synthesised pointer events actually reach the window server.

    Without the Accessibility permission the posts are silently dropped, and a
    whole run would then measure an application nobody touched. Moves the
    pointer a little and puts it back.
    """
    before = pointer_position()
    target = (before[0] + 20.0, before[1] + 20.0)
    move_pointer(target)
    time.sleep(0.25)
    after = pointer_position()
    move_pointer(before)

    return abs(after[0] - target[0]) < 2.0 and abs(after[1] - target[1]) < 2.0


# --- Per-process counters ---------------------------------------------------

# rusage_info_v0 is a 16-byte uuid followed by ten counters; the eight read
# here are the ones that matter. The prefix is fixed by the published ABI,
# which is why later flavours only ever append to it.
_RUSAGE_COUNTERS = struct.Struct("<8Q")
_RUSAGE_COUNTER_OFFSET = 16
_RUSAGE_INFO_V0 = 0


class _MachTimebase(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]


def _tick_nanoseconds():
    """Nanoseconds in one of the units the processor-time counters use.

    They are MACH ABSOLUTE TIME units, not nanoseconds, and the two differ by
    the machine: one to one on Intel, but 125/3 on Apple silicon. Reading them
    as nanoseconds understates processor time twenty-four fold on this machine
    and is exactly right on the other, which is the sort of mistake that gets
    believed.
    """
    timebase = _MachTimebase()
    if _libc.mach_timebase_info(ctypes.byref(timebase)) != 0 or timebase.denom == 0:
        return 1.0

    return timebase.numer / timebase.denom


_TICK_NANOSECONDS = _tick_nanoseconds()


class ProcessSample:
    """One reading of a process: cumulative processor time and memory.

    `cpu_nanoseconds` is user plus system time, so a difference of two samples
    over a wall-clock interval is the CORES of one core the process cost. Never
    report it as a percentage: Activity Monitor's percentages are per core and
    Task Manager's are per machine, and the two disagree by the core count.

    `footprint_bytes` is `phys_footprint`, the number Activity Monitor shows in
    its Memory column. It is NOT resident size - for this application the two
    differ by two orders of magnitude, because the graphics driver's arena is
    charged to the process without being mapped into it.
    """

    def __init__(self, cpu_nanoseconds, footprint_bytes, resident_bytes):
        self.cpu_nanoseconds = cpu_nanoseconds
        self.footprint_bytes = footprint_bytes
        self.resident_bytes = resident_bytes


def process_sample(pid):
    """Read a process's counters, or None if it has gone."""
    buffer = (ctypes.c_uint8 * 512)()
    if _libc.proc_pid_rusage(int(pid), _RUSAGE_INFO_V0, ctypes.byref(buffer)) != 0:
        return None
    user, system, _, _, _, _, resident, footprint = _RUSAGE_COUNTERS.unpack_from(bytes(buffer),
                                                                                _RUSAGE_COUNTER_OFFSET)

    return ProcessSample((user + system) * _TICK_NANOSECONDS, footprint, resident)
