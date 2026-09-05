"""Driving one scenario: put the application in a known state, act on it, and
measure what it cost.

Everything here is deliberately per-scenario. The application is launched fresh
for each one and killed afterwards, which costs a few seconds but means no
scenario inherits another's state - and it is the only way to drive two builds
that disagree about what a fresh launch even means.

What is measured, and in what units:

  cpu       CORES of one core: processor-time delta over wall-clock delta.
            Never a percentage - macOS reports percentages per core and Windows
            per machine, and reading one as the other has drawn a wrong
            conclusion on this project before.
  footprint MEGABYTES of phys_footprint, the figure Activity Monitor shows.
            Not resident size: for this application the two differ by two
            orders of magnitude, because the graphics driver's arena is charged
            to the process without being mapped into it.
  frames    Complete frame diagnostic lines observed per second over the
            full measurement window, including idle time.
  passes    Analysis passes a second, from the same place. Frames and passes
            are reported without a verdict: a build that keeps up where another
            dropped frames costs MORE processor time for a better result, so a
            movement in either direction has to be read, not judged.
"""

import math
import os
import re
import signal
import statistics
import subprocess
import threading
import time

from . import catalog, quartz

_DIAG_LINE = re.compile(r"^t=([0-9.]+) perf (frame|pass) ")

_LAUNCH_TIMEOUT_SECONDS = 25.0
_QUIT_TIMEOUT_SECONDS = 8.0
# Long enough for the capture stream, the first analysis pass and the window
# animation to be behind us, so a measurement sees the steady state.
SETTLE_SECONDS = 3.0
# A standard titled window's title bar, which is where the content window is
# grabbed to move it.
TITLE_BAR_HEIGHT = 28.0
# How long to wait for the first analysis pass after a region is set up, before
# concluding that the application never took it.
_REGION_CONFIRM_SECONDS = 3.0


class Measurement:
    def __init__(self, cores, footprint_mb, resident_mb, frames_per_second, passes_per_second, content_cores,
                 tracking=None, windows=None, measurement_method=None):
        self.cores = cores
        self.footprint_mb = footprint_mb
        self.resident_mb = resident_mb
        self.frames_per_second = frames_per_second
        self.passes_per_second = passes_per_second
        self.content_cores = content_cores
        # How far the region border trailed the pointer, for the actions that
        # drag it; None for every other scenario.
        self.tracking = tracking
        self.windows = windows
        self.measurement_method = measurement_method


class ScenarioResult:
    def __init__(self, scenario, stack, measurement=None, absent_reason="", warnings=()):
        self.scenario = scenario
        self.stack = stack
        self.measurement = measurement
        self.absent_reason = absent_reason
        self.warnings = list(warnings)


# --- Preferences ------------------------------------------------------------


# A key no build knows, so every one skips it, and the harness can recognise its
# own file. It matters because builds older than SIDESCOPES_PREFS_FILE write the
# real preferences file during a measured run, so a run killed outright leaves
# the harness's file in place - and the next run must not then back THAT up as
# though it were the user's.
HARNESS_MARKER = "scenario_harness=1"


# The quality level a measured launch starts at, for comparing the levels
# against each other on one binary. Unset leaves the key out entirely, so the
# application defaults it and a build that predates the setting is unaffected.
QUALITY_VARIABLE = "SIDESCOPES_SCENARIO_QUALITY"


def measurement_method(diagnostics_enabled):
    return {
        "version": "observed-window/2",
        "diagnostics": {"enabled": diagnostics_enabled,
                        "flush": "every-line" if diagnostics_enabled else "disabled"},
        # Empty means the build's default; do not guess the resolved quality.
        "quality_override": {QUALITY_VARIABLE: os.environ.get(QUALITY_VARIABLE, "")},
    }


def preferences_text(stack, window_rect):
    """The whole preferences file a measured launch starts from.

    Only the keys a scenario depends on are written. Both builds ignore keys
    they do not know and default the ones they do not find, which is what makes
    one file usable for either.
    """
    x, y, width, height = (round(value) for value in window_rect)
    quality = os.environ.get(QUALITY_VARIABLE, "").strip()
    quality_line = f"quality={quality}\n" if quality else ""

    scope_ids = "".join(f"[{catalog.SCOPE_LETTERS[letter]}]" for letter in dict.fromkeys(stack))
    return (f"{HARNESS_MARKER}\n"
            f"scope_stack={scope_ids}\n"
            f"{quality_line}"
            f"window_x={x}\n"
            f"window_y={y}\n"
            f"window_width={width}\n"
            f"window_height={height}\n")


# --- The application process ------------------------------------------------


def _executable(bundle):
    return bundle / "Contents" / "MacOS" / "SideScopes"


def _pgrep(pattern):
    finished = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True, check=False)

    return [int(line) for line in finished.stdout.split() if line.isdigit()]


def find_running(bundle):
    """Every process running this bundle's executable."""
    return _pgrep(str(_executable(bundle)))


def find_any_running():
    """Every SideScopes process, from whichever bundle.

    The harness must never measure, and never kill, an instance somebody else
    started - including one launched from a different copy of the application.
    """
    return _pgrep("SideScopes.app/Contents/MacOS/SideScopes")


def launch(bundle, environment):
    """Start the application and return its process id.

    Always through `open`, never the executable inside the bundle: launching
    the inner binary re-signs nothing but is enough to lose the screen
    recording permission, which is granted against the bundle.
    """
    arguments = ["open", "-n"]
    for name, value in environment.items():
        arguments += ["--env", f"{name}={value}"]
    arguments.append(str(bundle))
    finished = subprocess.run(arguments, capture_output=True, text=True, check=False)
    if finished.returncode != 0:
        raise RuntimeError(f"cannot launch {bundle}: {finished.stderr.strip()}")
    deadline = time.monotonic() + _LAUNCH_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        running = find_running(bundle)
        if running:
            return running[-1]
        time.sleep(0.2)
    raise RuntimeError(f"{bundle} did not start")


def quit_application(pid):
    """Stop a measured launch.

    A signal rather than the quit shortcut, so that the application never
    writes its own state over the harness's preferences file on the way out.
    """
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + _QUIT_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if quartz.process_sample(pid) is None:
            return
        time.sleep(0.2)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


# --- The diagnostic stream --------------------------------------------------


class DiagnosticTail:
    """Counts complete lines becoming visible between file-size snapshots.

    Launches use every-line flushing. These are observation boundaries, not
    exact source-event boundaries: an in-flight write can straddle either end.
    The diagnostic clock starts inside the app and cannot time our CPU samples.
    """

    def __init__(self, path):
        self.path = path
        self._start = None

    def _snapshot(self):
        before = time.monotonic()
        try:
            with self.path.open("rb") as handle:
                state = os.fstat(handle.fileno())
                after = time.monotonic()
                # Later appends cannot enter this observation.
                data = handle.read(state.st_size)
                if len(data) != state.st_size:
                    raise RuntimeError("diagnostic log changed while being read")
                identity = (state.st_dev, state.st_ino)
        except FileNotFoundError:
            after = time.monotonic()
            data, identity = b"", None
        return data, identity, ((before + after) / 2.0, after - before)

    def mark(self):
        self._start = self._snapshot()

    def finish(self):
        initial, identity, start = self._start
        final, final_identity, end = self._snapshot()
        if final_identity is None:
            raise RuntimeError("diagnostic log was not produced")
        if (identity is not None and identity != final_identity) or not final.startswith(initial):
            raise RuntimeError("diagnostic log rotated or was rewritten during measurement")
        # A partial line at either boundary belongs to the window in which
        # its newline becomes visible, even if its source timestamp is earlier.
        first = initial.rfind(b"\n") + 1
        last = final.rfind(b"\n") + 1
        counts = {"frame": 0, "pass": 0}
        for line in final[first:last].decode(errors="replace").splitlines():
            found = _DIAG_LINE.match(line)
            if found:
                counts[found.group(2)] += 1
        return dict(_window(start, end), counts=counts,
                    start_byte=first, end_byte=last)


def _window(start, end):
    duration = end[0] - start[0]
    if duration <= 0:
        raise RuntimeError("measurement window has no positive duration")
    return {"start_seconds": start[0], "end_seconds": end[0], "duration_seconds": duration,
            "start_sample_span_seconds": start[1], "end_sample_span_seconds": end[1]}


def _process_sample(pid):
    before = time.monotonic()
    sample = quartz.process_sample(pid)
    after = time.monotonic()
    return sample, ((before + after) / 2.0, after - before)


# --- What the harness does while measuring ----------------------------------


class Action:
    """A repeated interaction, run on its own thread for a scenario's length."""

    def __init__(self):
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=10.0)

    def complaints(self):
        """What went wrong while acting, so a scenario that did not really run
        says so rather than reporting the cost of doing nothing."""
        return []

    def _loop(self):
        raise NotImplementedError


class Target:
    """What an action needs to know about the application it is driving."""

    def __init__(self, pid=None, bundle=None, bindings=None):
        self.pid = pid
        self.bundle = bundle
        self.bindings = bindings or {"draw": "d", "attach": "a"}


class Still(Action):
    def start(self):
        pass

    def stop(self):
        pass


class PointerSweep(Action):
    """Sweeps the pointer across the region on a path that never repeats.

    The path matters. An earlier measurement circled a patch of uniform content,
    so the colour under the pointer barely changed and the readout looked free;
    a Lissajous figure whose two frequencies do not divide each other crosses
    the whole rectangle and keeps finding new pixels.
    """

    def __init__(self, rect, steps_per_second=60.0):
        super().__init__()
        self._rect = rect
        self._interval = 1.0 / steps_per_second

    def _loop(self):
        x, y, width, height = self._rect
        centre = (x + (width / 2.0), y + (height / 2.0))
        amplitude = (width * 0.45, height * 0.45)
        step = 0
        while not self._stop.is_set():
            phase = step * 0.05
            quartz.move_pointer((centre[0] + (amplitude[0] * math.cos(phase)),
                                 centre[1] + (amplitude[1] * math.sin(phase * 1.37))))
            step += 1
            time.sleep(self._interval)


class BorderDrag(Action):
    """Drags the region back and forth by its border, scanning the content.

    The grab has to land on the border band just outside the region - the band
    is twelve points wide, and a quarter of the way along an edge is clear of
    the midpoint handles that resize instead of move.
    """

    def __init__(self, region, distance):
        super().__init__()
        self._region = region
        self._distance = distance

    def _loop(self):
        x, y, width, _ = self._region
        grab = [x + (width * 0.25), y - 6.0]
        forward = True
        while not self._stop.is_set():
            offset = self._distance if forward else -self._distance
            target = (grab[0] + offset, grab[1])
            quartz.drag(tuple(grab), target, steps=40, step_seconds=0.02)
            grab = [target[0], target[1]]
            forward = not forward


class Tracking:
    """How closely the region border followed the pointer.

    Read off the geometry rather than out of the application: during a sweep at
    a known velocity the distance the border is behind the pointer IS the time
    it is behind, times that velocity. Both are sampled from the window server,
    so this measures what the user sees and needs nothing compiled in.

    `settle_ms` is the other half of the same question - how long after the
    button is released the border stops where the pointer left it.
    """

    def __init__(self, lags_ms, settles_ms):
        ordered = sorted(lags_ms)
        self.samples = len(ordered)
        self.median_ms = ordered[len(ordered) // 2] if ordered else None
        self.worst_ms = ordered[-1] if ordered else None
        self.settle_ms = statistics.median(settles_ms) if settles_ms else None


# The border window extends this far beyond the region on every side, and
# carries a label strip above it. Both come from the platform overlay, and the
# harness needs them only to recognise the window in the list.
BORDER_PAD = 17.5
BORDER_LABEL_BAND = 20.0
# How often the border is sampled during a flick. Fast enough to resolve a
# frame period several times over, slow enough that the harness's own thread
# does not compete with the application it is measuring.
TRACK_SAMPLE_SECONDS = 0.004


def border_window(pid, region_size):
    """The region border's frame, recognised by its size around the region."""
    want = (region_size[0] + 2 * BORDER_PAD, region_size[1] + 2 * BORDER_PAD + BORDER_LABEL_BAND)
    best = None
    for x, y, width, height in quartz.windows_owned_by(pid):
        error = abs(width - want[0]) + abs(height - want[1])
        if error < 90.0 and (best is None or error < best[0]):
            best = (error, (x, y, width, height))

    return best[1] if best else None


class BorderFlick(Action):
    """Throws the region across the picture, the way one is really moved.

    BorderDrag above sweeps slowly and evenly over most of a second, which is
    how nobody moves a region. A region is drawn in well under a second and then
    flicked from one face to another - "a quick, rough move, not a precise
    selection" - so this one throws it: a short burst of large steps, a pause,
    and the same back again. It samples the border against the pointer
    throughout, because the question this scenario asks is whether the border
    kept up, and no cost figure answers that.
    """

    def __init__(self, target, region, distance, seconds=0.2, steps=12, rest=0.35):
        super().__init__()
        self._pid = target.pid
        self._size = (region[2], region[3])
        self._distance = distance
        self._seconds = seconds
        self._steps = steps
        self._rest = rest
        self._lags = []
        self._settles = []
        self._missed = 0

    def tracking(self):
        return Tracking(self._lags, self._settles)

    def complaints(self):
        if self._missed:
            return [f"the border was missing for {self._missed} of the flicks, which were not measured"]

        return []

    def _loop(self):
        forward = True
        while not self._stop.is_set():
            self._flick(self._distance if forward else -self._distance)
            forward = not forward
            self._stop.wait(self._rest)

    def _flick(self, distance):
        frame = border_window(self._pid, self._size)
        if frame is None:
            self._missed += 1

            return
        # A quarter along the top band: outside the region, clear of both the
        # corner zones and the edge midpoint, so the grab moves rather than
        # resizes.
        grab = (frame[0] + BORDER_PAD + (self._size[0] * 0.25), frame[1] + BORDER_LABEL_BAND + BORDER_PAD - 6.0)
        quartz.move_pointer(grab)
        time.sleep(0.08)
        quartz.press_mouse(grab)
        time.sleep(0.08)
        origin = border_window(self._pid, self._size)
        if origin is None:
            quartz.release_mouse(grab)
            self._missed += 1

            return
        started = time.monotonic()
        for step in range(1, self._steps + 1):
            due = started + (self._seconds * step / self._steps)
            quartz.drag_mouse((grab[0] + (distance * step / self._steps), grab[1]))
            self._sample(origin, grab, distance / self._seconds, due)
        quartz.release_mouse((grab[0] + distance, grab[1]))
        self._settles.append(self._settle(origin, grab, distance))

    def _sample(self, origin, grab, velocity, until):
        """Collect the border's lag behind the pointer until a moment passes."""
        while time.monotonic() < until:
            frame = border_window(self._pid, self._size)
            if frame is not None:
                behind = (quartz.pointer_position()[0] - grab[0]) - (frame[0] - origin[0])
                self._lags.append(behind / velocity * 1000.0)
            time.sleep(TRACK_SAMPLE_SECONDS)

    def _settle(self, origin, grab, distance):
        """Milliseconds from the release until the border stops where it was left."""
        released = time.monotonic()
        while time.monotonic() - released < 1.0:
            frame = border_window(self._pid, self._size)
            if frame is not None and abs((frame[0] - origin[0]) - distance) < 2.0:
                return (time.monotonic() - released) * 1000.0
            time.sleep(TRACK_SAMPLE_SECONDS)

        return 1000.0


class RegionRedraw(Action):
    """Draws a region roughly and quickly, clears it, and draws it again.

    The other half of what the owner does: "the region is often drawn within a
    second". Nothing here is a careful selection - the picker is opened, a
    rectangle is thrown across the content in a fifth of a second, and Escape
    takes it away again. What it exercises is the picker's own rubber band,
    which is drawn by the overlay rather than by the application's frame, and
    the settings churn a growing region pushes at the worker.
    """

    def __init__(self, target, region, rest=0.5):
        super().__init__()
        self._target = target
        self._region = region
        self._rest = rest
        self._drawn = 0
        self._attempts = 0

    def complaints(self):
        if self._attempts and not self._drawn:
            return ["no region was ever drawn, so this measured an application nobody drew on"]

        return []

    def _loop(self):
        x, y, width, height = self._region
        corners = ((x, y), (x + width, y + height))
        while not self._stop.is_set():
            # The picker takes the keyboard and Escape hands it back, but a
            # letter pressed while something else holds it is simply lost - so
            # the application is brought forward for each cycle, exactly as the
            # region setup does.
            activate(self._target.bundle)
            self._attempts += 1
            quartz.press_key(self._target.bindings["draw"])
            self._stop.wait(0.35)
            quartz.drag(corners[0], corners[1], steps=12, step_seconds=0.016, settle=0.08)
            self._stop.wait(self._rest)
            if border_window(self._target.pid, (width, height)) is not None:
                self._drawn += 1
            quartz.press_key("escape")
            self._stop.wait(self._rest)


class WindowDrag(Action):
    """Drags the content window itself, the way a person moves an editor.

    By its title bar, which sits just above the content rectangle. The window
    deliberately does not follow a drag across its content, so that a region
    drawn over it cannot move it by accident.
    """

    def __init__(self, content_rect, distance):
        super().__init__()
        self._point = [content_rect[0] + (content_rect[2] * 0.5), content_rect[1] - (TITLE_BAR_HEIGHT / 2.0)]
        self._distance = distance

    def _loop(self):
        forward = True
        while not self._stop.is_set():
            offset = self._distance if forward else -self._distance
            target = (self._point[0] + offset, self._point[1])
            quartz.drag(tuple(self._point), target, steps=40, step_seconds=0.02)
            self._point = [target[0], target[1]]
            forward = not forward


def action_for(name, region, content_rect, target=None):
    target = target or Target()
    if name == "pointer-sweep":
        return PointerSweep(region)
    if name == "region-drag":
        return BorderDrag(region, distance=min(200.0, content_rect[2] * 0.15))
    if name == "region-flick":
        # Far enough to be a throw across the picture rather than a nudge, and
        # no further than keeps the region on the display it was drawn on.
        return BorderFlick(target, region, distance=min(300.0, content_rect[2] * 0.3))
    if name == "region-redraw":
        return RegionRedraw(target, region)
    if name == "window-drag":
        return WindowDrag(content_rect, distance=min(200.0, content_rect[2] * 0.15))

    return Still()


# --- Setting up a region ----------------------------------------------------


def _centre_of(rect):
    return (rect[0] + (rect[2] / 2.0), rect[1] + (rect[3] / 2.0))


def activate(bundle):
    """Bring the measured application forward, so a shortcut reaches it.

    Without this the harness lost roughly one shortcut in eight: a plain letter
    goes to whatever holds the keyboard, and the previous scenario's application
    closing hands focus to something else. `open` without -n activates the
    instance already running rather than starting another.
    """
    subprocess.run(["open", str(bundle)], capture_output=True, check=False)
    time.sleep(0.4)


def _perform_region(kind, region, content_rect, bindings, bundle=None):
    if bundle is not None:
        activate(bundle)
    if kind == "draw":
        quartz.press_key(bindings["draw"])
        time.sleep(0.6)
        quartz.drag((region[0], region[1]), (region[0] + region[2], region[1] + region[3]), steps=25)
        time.sleep(0.8)
    elif kind == "attach":
        quartz.press_key(bindings["attach"])
        time.sleep(0.8)
        quartz.click(_centre_of(content_rect))
        time.sleep(1.2)


def await_window(pid, expected_size, timeout_seconds=20.0):
    """Wait until the application's own window is on screen at about its size.

    A better readiness signal than a rendered frame: a shortcut pressed before
    the window exists is simply discarded, and the scenario then measures an
    application in the wrong state.
    """
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        for _, _, width, height in quartz.windows_owned_by(pid):
            if abs(width - expected_size[0]) <= 40.0 and abs(height - expected_size[1]) <= 80.0:
                return True
        time.sleep(0.3)

    return False


def region_border_visible(pid, expected):
    """Whether the application is showing a region border of about this size.

    The border is a window of its own, sitting a few points outside the region,
    so the window list confirms a region in any version of the application. The
    tolerance covers that inset and the rounding a drawn rectangle picks up.

    Deliberately NOT confirmed by watching for analysis passes: a region over
    content that does not change runs no passes at all - correct behaviour, and
    indistinguishable from having no region.
    """
    slack = (max(80.0, expected[2] * 0.1), max(80.0, expected[3] * 0.1))
    for _, _, width, height in quartz.windows_owned_by(pid):
        if abs(width - expected[2]) <= slack[0] and abs(height - expected[3]) <= slack[1]:
            return True

    return False


def _await_border(pid, expected):
    deadline = time.monotonic() + _REGION_CONFIRM_SECONDS
    while time.monotonic() < deadline:
        if region_border_visible(pid, expected):
            return True
        time.sleep(0.3)

    return False


def establish_region(pid, kind, region, content_rect, bindings, bundle=None, attempts=3):
    """Put the application on a region, through its own interface.

    The region is not persisted, so it cannot be written into a preferences
    file: it has to be drawn or attached the way a person would.

    The result is CONFIRMED rather than assumed, and tried again if it did not
    take. A shortcut pressed before the window is ready is simply lost, and a
    scenario that then measured an application with no region at all would read
    as a spectacular improvement.

    @return Whether the application ended up on a region.
    """
    if kind == "none":
        # Current builds start with a starter region. Establish the requested
        # empty state explicitly; startup-default bypasses this setup.
        if bundle is not None:
            activate(bundle)
        quartz.press_key(bindings.get("clear", "escape"))
        return True
    expected = region if kind == "draw" else content_rect
    for attempt in range(attempts):
        _perform_region(kind, region, content_rect, bindings, bundle)
        if _await_border(pid, expected):
            return True
        time.sleep(1.0 + attempt)

    return False


# --- One scenario -----------------------------------------------------------


def measure(pid, content_pid, seconds, tail=None, sample_seconds=0.5, action=None):
    """Watch a running application for a while and report what it cost."""
    if tail is not None:
        tail.mark()
    started = time.monotonic()
    first, app_start = _process_sample(pid)
    content_first, content_start = _process_sample(content_pid) if content_pid else (None, None)
    peak_footprint = first.footprint_bytes if first else 0
    peak_resident = first.resident_bytes if first else 0
    while time.monotonic() - started < seconds:
        time.sleep(sample_seconds)
        sample = quartz.process_sample(pid)
        if sample is None:
            raise RuntimeError("the application exited during the measurement")
        peak_footprint = max(peak_footprint, sample.footprint_bytes)
        peak_resident = max(peak_resident, sample.resident_bytes)
    last, app_end = _process_sample(pid)
    content_last, content_end = _process_sample(content_pid) if content_pid else (None, None)
    if first is None or last is None:
        raise RuntimeError("the application could not be sampled")
    peak_footprint = max(peak_footprint, last.footprint_bytes)
    peak_resident = max(peak_resident, last.resident_bytes)
    if content_pid and (content_first is None or content_last is None):
        raise RuntimeError("the content window could not be sampled")
    app_window = _window(app_start, app_end)
    content_window = _window(content_start, content_end) if content_pid else None
    diagnostic_window = tail.finish() if tail is not None else None
    frames = diagnostic_window["counts"]["frame"] / diagnostic_window["duration_seconds"] if tail else 0.0
    passes = diagnostic_window["counts"]["pass"] / diagnostic_window["duration_seconds"] if tail else 0.0
    content_cores = 0.0
    if content_window is not None:
        content_cores = ((content_last.cpu_nanoseconds - content_first.cpu_nanoseconds)
                         / (content_window["duration_seconds"] * 1e9))

    return Measurement(
        cores=(last.cpu_nanoseconds - first.cpu_nanoseconds) / (app_window["duration_seconds"] * 1e9),
        footprint_mb=peak_footprint / 1e6,
        resident_mb=peak_resident / 1e6,
        frames_per_second=frames,
        passes_per_second=passes,
        content_cores=content_cores,
        tracking=action.tracking() if hasattr(action, "tracking") else None,
        windows={"cpu": app_window, "content-cpu": content_window, "diagnostics": diagnostic_window},
        measurement_method=measurement_method(tail is not None),
    )
