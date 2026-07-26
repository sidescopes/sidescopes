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
  frames    Rendered frames a second, counted from the application's own
            diagnostic channel and timed by its own clock.
  passes    Analysis passes a second, from the same place. Frames and passes
            are reported without a verdict: a build that keeps up where another
            dropped frames costs MORE processor time for a better result, so a
            movement in either direction has to be read, not judged.
"""

import math
import os
import re
import signal
import subprocess
import threading
import time

from . import quartz

_DIAG_LINE = re.compile(r"^t=([0-9.]+) perf (frame|pass) ")

_LAUNCH_TIMEOUT_SECONDS = 25.0
_QUIT_TIMEOUT_SECONDS = 8.0
# Long enough for the capture stream, the first analysis pass and the window
# animation to be behind us, so a measurement sees the steady state.
SETTLE_SECONDS = 3.0
# The diagnostic sink flushes on an interval; waiting past it means the last
# lines of a window are counted rather than the next window's first.
_FLUSH_SECONDS = 0.3
# A standard titled window's title bar, which is where the content window is
# grabbed to move it.
TITLE_BAR_HEIGHT = 28.0
# How long to wait for the first analysis pass after a region is set up, before
# concluding that the application never took it.
_REGION_CONFIRM_SECONDS = 3.0


class Measurement:
    def __init__(self, cores, footprint_mb, resident_mb, frames_per_second, passes_per_second, content_cores):
        self.cores = cores
        self.footprint_mb = footprint_mb
        self.resident_mb = resident_mb
        self.frames_per_second = frames_per_second
        self.passes_per_second = passes_per_second
        self.content_cores = content_cores


class ScenarioResult:
    def __init__(self, scenario, stack, measurement=None, absent_reason="", warnings=()):
        self.scenario = scenario
        self.stack = stack
        self.measurement = measurement
        self.absent_reason = absent_reason
        self.warnings = list(warnings)


# --- Preferences ------------------------------------------------------------


def preferences_text(stack, window_rect):
    """The whole preferences file a measured launch starts from.

    Only the keys a scenario depends on are written. Both builds ignore keys
    they do not know and default the ones they do not find, which is what makes
    one file usable for either.
    """
    x, y, width, height = (round(value) for value in window_rect)

    return (f"scope_stack={stack}\n"
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
    """Counts the application's own frame and pass lines over a window.

    Rates come from the timestamps the application wrote, not from the
    harness's clock, so a flush landing either side of a boundary cannot skew
    them.
    """

    def __init__(self, path):
        self.path = path
        self._offset = 0

    def mark(self):
        self._offset = self.path.stat().st_size if self.path.exists() else 0

    def rates(self):
        if not self.path.exists():
            return (0.0, 0.0)
        with open(self.path, "r", errors="replace") as handle:
            handle.seek(self._offset)
            block = handle.read()
        stamps = {"frame": [], "pass": []}
        for line in block.splitlines():
            found = _DIAG_LINE.match(line)
            if found:
                stamps[found.group(2)].append(float(found.group(1)))

        return (_rate(stamps["frame"]), _rate(stamps["pass"]))


def _rate(stamps):
    if len(stamps) < 2:
        return 0.0
    span = stamps[-1] - stamps[0]

    return (len(stamps) - 1) / span if span > 0 else 0.0


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

    def _loop(self):
        raise NotImplementedError


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


def action_for(name, region, content_rect):
    if name == "pointer-sweep":
        return PointerSweep(region)
    if name == "region-drag":
        return BorderDrag(region, distance=min(200.0, content_rect[2] * 0.15))
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
        return True
    expected = region if kind == "draw" else content_rect
    for attempt in range(attempts):
        _perform_region(kind, region, content_rect, bindings, bundle)
        if _await_border(pid, expected):
            return True
        time.sleep(1.0 + attempt)

    return False


# --- One scenario -----------------------------------------------------------


def measure(pid, content_pid, seconds, tail=None, sample_seconds=0.5):
    """Watch a running application for a while and report what it cost."""
    if tail is not None:
        tail.mark()
    started = time.monotonic()
    first = quartz.process_sample(pid)
    content_first = quartz.process_sample(content_pid) if content_pid else None
    peak_footprint = first.footprint_bytes if first else 0
    peak_resident = first.resident_bytes if first else 0
    while time.monotonic() - started < seconds:
        time.sleep(sample_seconds)
        sample = quartz.process_sample(pid)
        if sample is None:
            raise RuntimeError("the application exited during the measurement")
        peak_footprint = max(peak_footprint, sample.footprint_bytes)
        peak_resident = max(peak_resident, sample.resident_bytes)
    elapsed = time.monotonic() - started
    last = quartz.process_sample(pid)
    if first is None or last is None:
        raise RuntimeError("the application could not be sampled")
    time.sleep(_FLUSH_SECONDS)
    frames, passes = tail.rates() if tail is not None else (0.0, 0.0)
    content_cores = 0.0
    if content_first is not None:
        content_last = quartz.process_sample(content_pid)
        if content_last is not None:
            content_cores = (content_last.cpu_nanoseconds - content_first.cpu_nanoseconds) / (elapsed * 1e9)

    return Measurement(
        cores=(last.cpu_nanoseconds - first.cpu_nanoseconds) / (elapsed * 1e9),
        footprint_mb=peak_footprint / 1e6,
        resident_mb=peak_resident / 1e6,
        frames_per_second=frames,
        passes_per_second=passes,
        content_cores=content_cores,
    )
