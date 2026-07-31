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
import statistics
import subprocess
import sys
import threading
import time

from . import catalog
from . import desktop

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
    def __init__(self, cores, footprint_mb, resident_mb, frames_per_second, passes_per_second, content_cores,
                 tracking=None):
        self.cores = cores
        self.footprint_mb = footprint_mb
        self.resident_mb = resident_mb
        self.frames_per_second = frames_per_second
        self.passes_per_second = passes_per_second
        self.content_cores = content_cores
        # How far the region border trailed the pointer, for the actions that
        # drag it; None for every other scenario.
        self.tracking = tracking


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


def scope_stack_setting(stack):
    """The stack letters as the preferences file spells a stack.

    The application keeps only BRACKETED ids from this key (cleanedScopeStack
    in core/preferences.cpp) and ignores everything else, so the bare letters
    a scenario names parse to nothing and the run silently measures whichever
    stack the application defaults to. Writing the ids is what makes the
    --stacks argument mean anything.
    """
    return "".join(f"[{catalog.SCOPE_LETTERS[letter]}]" for letter in stack if letter in catalog.SCOPE_LETTERS)


def preferences_text(stack, window_rect):
    """The whole preferences file a measured launch starts from.

    Only the keys a scenario depends on are written. Both builds ignore keys
    they do not know and default the ones they do not find, which is what makes
    one file usable for either.
    """
    x, y, width, height = (round(value) for value in window_rect)
    quality = os.environ.get(QUALITY_VARIABLE, "").strip()
    quality_line = f"quality={quality}\n" if quality else ""

    return (f"{HARNESS_MARKER}\n"
            f"scope_stack={scope_stack_setting(stack)}\n"
            f"{quality_line}"
            f"window_x={x}\n"
            f"window_y={y}\n"
            f"window_width={width}\n"
            f"window_height={height}\n")


# --- The application process ------------------------------------------------


# The two spellings a built application answers to: the binary in a build tree
# is SideScopes, an installed one is sidescopes, and the repository's naming rule
# is what makes those the only two.
_EXECUTABLE_NAMES = ("SideScopes", "sidescopes")


def executable_in(application_path):
    """The binary inside a built application, however this system packages one.

    macOS hands out a bundle with the executable inside it. Linux hands out the
    executable itself from a build tree, or an installed tree with it under
    bin/ - and a path that already IS the binary is taken as given, which is
    what a run against `build/SideScopes` passes.
    """
    if sys.platform == "darwin":
        return application_path / "Contents" / "MacOS" / "SideScopes"
    if not application_path.is_dir():
        return application_path
    installed = application_path / "bin" / "sidescopes"

    return installed if installed.exists() else application_path / "SideScopes"


def _pgrep(pattern):
    finished = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True, check=False)

    return [int(line) for line in finished.stdout.split() if line.isdigit()]


def _executed_file(pid):
    """The file a process is running, or an empty string where it cannot be read.

    /proc/<pid>/exe rather than the command line: it is the kernel's own link to
    what was executed, so a process cannot argue with it by rewriting its
    arguments and a launch through a wrapper still names the binary. A build
    REPLACED since it was launched - a rebuild during a session - reads with a
    " (deleted)" suffix, which is stripped, because a running old build is
    still an instance the harness must not measure around.
    """
    try:
        path = os.readlink(f"/proc/{pid}/exe")
    except OSError:
        return ""

    return path.removesuffix(" (deleted)")


def _processes():
    """Every process this user can see, as (pid, the file it is running)."""
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        running = _executed_file(int(entry))
        if running:
            yield (int(entry), running)


def find_running(application_path):
    """Every process running this build's executable."""
    if sys.platform == "darwin":
        return _pgrep(str(executable_in(application_path)))
    wanted = str(executable_in(application_path).resolve())

    return [pid for pid, running in _processes() if running == wanted]


def find_any_running():
    """Every SideScopes process, from whichever build.

    The harness must never measure, and never kill, an instance somebody else
    started - including one launched from a different copy of the application.
    """
    if sys.platform == "darwin":
        return _pgrep("SideScopes.app/Contents/MacOS/SideScopes")

    return [pid for pid, running in _processes() if os.path.basename(running) in _EXECUTABLE_NAMES]


def launch(application_path, environment):
    """Start the application and return its process id."""
    if sys.platform == "darwin":
        return _launch_bundle(application_path, environment)

    return _launch_executable(executable_in(application_path), environment)


def _launch_bundle(bundle, environment):
    """Start a macOS bundle.

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


def _launch_executable(executable, environment):
    """Run the binary itself, which is all Linux offers - there is no service
    that starts an application by name and hands it an environment.

    The environment is the harness's own with the run's keys added, exactly as
    `open --env` adds them, and then put through the platform layer, which
    settles which display system the application will take. In a session of its
    own so that a signal to the harness's process group does not take the
    measured application with it; its stderr is left where the operator can see
    it, because an application refusing to start says so there.
    """
    prepared = desktop.application_environment(dict(os.environ, **environment))
    started = subprocess.Popen([str(executable)], env=prepared, start_new_session=True, stdout=subprocess.DEVNULL)
    deadline = time.monotonic() + _LAUNCH_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if started.poll() is not None:
            raise RuntimeError(f"{executable} exited at once with status {started.returncode}")
        if desktop.process_sample(started.pid) is not None:
            return started.pid
        time.sleep(0.05)
    raise RuntimeError(f"{executable} did not start")


def _reap(pid):
    """Collect a launch the harness made itself, so that a killed scenario
    leaves no zombie for the next one's "is something already running" probe to
    reason about. On macOS the application is `open`'s child rather than the
    harness's and there is nothing here to take."""
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass


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
        if desktop.process_sample(pid) is None:
            _reap(pid)

            return
        time.sleep(0.2)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    _reap(pid)


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

    def complaints(self):
        """What went wrong while acting, so a scenario that did not really run
        says so rather than reporting the cost of doing nothing."""
        return []

    def _loop(self):
        raise NotImplementedError


class Target:
    """What an action needs to know about the application it is driving."""

    def __init__(self, pid=None, application=None, bindings=None):
        self.pid = pid
        self.application = application
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
            desktop.move_pointer((centre[0] + (amplitude[0] * math.cos(phase)),
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
            desktop.drag(tuple(grab), target, steps=40, step_seconds=0.02)
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
    for x, y, width, height in desktop.windows_owned_by(pid):
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
        desktop.move_pointer(grab)
        time.sleep(0.08)
        desktop.press_mouse(grab)
        time.sleep(0.08)
        origin = border_window(self._pid, self._size)
        if origin is None:
            desktop.release_mouse(grab)
            self._missed += 1

            return
        started = time.monotonic()
        for step in range(1, self._steps + 1):
            due = started + (self._seconds * step / self._steps)
            desktop.drag_mouse((grab[0] + (distance * step / self._steps), grab[1]))
            self._sample(origin, grab, distance / self._seconds, due)
        desktop.release_mouse((grab[0] + distance, grab[1]))
        self._settles.append(self._settle(origin, grab, distance))

    def _sample(self, origin, grab, velocity, until):
        """Collect the border's lag behind the pointer until a moment passes."""
        while time.monotonic() < until:
            frame = border_window(self._pid, self._size)
            if frame is not None:
                behind = (desktop.pointer_position()[0] - grab[0]) - (frame[0] - origin[0])
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
            activate(self._target.application, self._target.pid)
            self._attempts += 1
            desktop.press_key(self._target.bindings["draw"])
            self._stop.wait(0.35)
            desktop.drag(corners[0], corners[1], steps=12, step_seconds=0.016, settle=0.08)
            self._stop.wait(self._rest)
            if border_window(self._target.pid, (width, height)) is not None:
                self._drawn += 1
            desktop.press_key("escape")
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
            desktop.drag(tuple(self._point), target, steps=40, step_seconds=0.02)
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


def activate(application_path, pid=None):
    """Bring the measured application forward, so a shortcut reaches it.

    Without this the harness lost roughly one shortcut in eight: a plain letter
    goes to whatever holds the keyboard, and the previous scenario's application
    closing hands focus to something else.

    Named by whichever each system can be asked about. macOS is asked about the
    application: `open` without -n activates the instance already running rather
    than starting another. X knows only windows, so it is asked about the
    process's own window instead - the platform layer decides how, since a
    window manager owns the answer where one is running.
    """
    if sys.platform == "darwin":
        subprocess.run(["open", str(application_path)], capture_output=True, check=False)
    elif pid is not None:
        desktop.activate_window(pid)
    time.sleep(0.4)


def _perform_region(kind, region, content_rect, bindings, application_path=None, pid=None):
    if application_path is not None:
        activate(application_path, pid)
    if kind == "draw":
        desktop.press_key(bindings["draw"])
        time.sleep(0.6)
        desktop.drag((region[0], region[1]), (region[0] + region[2], region[1] + region[3]), steps=25)
        time.sleep(0.8)
    elif kind == "attach":
        desktop.press_key(bindings["attach"])
        time.sleep(0.8)
        desktop.click(_centre_of(content_rect))
        time.sleep(1.2)


def await_window(pid, expected_size, timeout_seconds=20.0):
    """Wait until the application's own window is on screen at about its size.

    A better readiness signal than a rendered frame: a shortcut pressed before
    the window exists is simply discarded, and the scenario then measures an
    application in the wrong state.
    """
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        for _, _, width, height in desktop.windows_owned_by(pid):
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
    for _, _, width, height in desktop.windows_owned_by(pid):
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


def establish_region(pid, kind, region, content_rect, bindings, application_path=None, attempts=3):
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
        _perform_region(kind, region, content_rect, bindings, application_path, pid)
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
    first = desktop.process_sample(pid)
    content_first = desktop.process_sample(content_pid) if content_pid else None
    peak_footprint = first.footprint_bytes if first else 0
    peak_resident = first.resident_bytes if first else 0
    while time.monotonic() - started < seconds:
        time.sleep(sample_seconds)
        sample = desktop.process_sample(pid)
        if sample is None:
            raise RuntimeError("the application exited during the measurement")
        peak_footprint = max(peak_footprint, sample.footprint_bytes)
        peak_resident = max(peak_resident, sample.resident_bytes)
    elapsed = time.monotonic() - started
    last = desktop.process_sample(pid)
    if first is None or last is None:
        raise RuntimeError("the application could not be sampled")
    time.sleep(_FLUSH_SECONDS)
    frames, passes = tail.rates() if tail is not None else (0.0, 0.0)
    content_cores = 0.0
    if content_first is not None:
        content_last = desktop.process_sample(content_pid)
        if content_last is not None:
            content_cores = (content_last.cpu_nanoseconds - content_first.cpu_nanoseconds) / (elapsed * 1e9)

    return Measurement(
        cores=(last.cpu_nanoseconds - first.cpu_nanoseconds) / (elapsed * 1e9),
        footprint_mb=peak_footprint / 1e6,
        resident_mb=peak_resident / 1e6,
        frames_per_second=frames,
        passes_per_second=passes,
        content_cores=content_cores,
        tracking=action.tracking() if hasattr(action, "tracking") else None,
    )
