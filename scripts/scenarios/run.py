"""Runs the scenario harness over a built application and writes its results.

    scripts/app-scenarios.sh --app build/SideScopes.app --out results.json

The output is one JSON document per run: the conditions it was taken under, the
measurements, and - the point of the exercise - an explicit list of the
scenarios this build could NOT be asked, with the reason. Compare two of them
with scripts/bench-compare.py, which refuses to put a number on anything the
two runs did not do identically.

Requires the Accessibility permission for whichever application runs this
script, since it drives the pointer and the keyboard: System Settings > Privacy
& Security > Accessibility. Without it, nothing moves and the harness stops
rather than measuring an application nobody touched.
"""

import argparse
import json
import pathlib
import shutil
import sys
import time

from . import catalog
from . import conditions
from . import content as content_module
from . import quartz
from . import session

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
PREFERENCES = pathlib.Path.home() / "Library" / "Application Support" / "SideScopes" / "preferences.txt"

# The shortcuts the harness presses. They are the application's defaults, and
# the preferences the harness writes never rebind them.
BINDINGS = {"draw": "d", "attach": "a"}


def _parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", type=pathlib.Path, default=REPOSITORY / "build" / "SideScopes.app",
                        help="the application bundle to measure")
    parser.add_argument("--out", type=pathlib.Path, help="where to write the results (default under bench-results/)")
    parser.add_argument("--scenarios", default="", help="comma list of scenario names; default every one")
    parser.add_argument("--stacks", default=",".join(catalog.DEFAULT_STACKS),
                        help="comma list of scope stacks to measure each scenario with")
    parser.add_argument("--seconds", type=float, help="override every scenario's measurement window")
    parser.add_argument("--region-pixels", default="x".join(str(value) for value in catalog.DEFAULT_REGION_PIXELS),
                        help="the captured size of the measurement region, WxH")
    parser.add_argument("--display", type=int, help="display id to run on; default the main display")
    parser.add_argument("--photographs", action="store_true",
                        help="fetch the manifest photographs instead of generating content")
    parser.add_argument("--no-diagnostics", action="store_true",
                        help="do not ask the application for frame and pass timings")
    parser.add_argument("--list", action="store_true", help="print the catalogue and exit")

    return parser.parse_args(argv)


def _list_catalogue():
    print("scenarios:")
    for scenario in catalog.SCENARIOS:
        marks = []
        if scenario.needs:
            marks.append("needs " + ", ".join(sorted(scenario.needs)))
        if not scenario.comparable:
            marks.append("NOT COMPARABLE across builds")
        suffix = f"  [{'; '.join(marks)}]" if marks else ""
        print(f"  {scenario.id:<22} {scenario.summary}{suffix}")
    print("\nstacks are strings of scope letters: " + ", ".join(
        f"{letter} {identifier.rsplit('.', 1)[1]}" for letter, identifier in catalog.SCOPE_LETTERS.items()))


def _target_display(wanted):
    found = quartz.displays()
    if not found:
        raise RuntimeError("no active display")
    if wanted is not None:
        for display in found:
            if display["id"] == wanted:
                return display
        raise RuntimeError(f"no display with id {wanted}")

    return next((display for display in found if display["main"]), found[0])


class PreferencesGuard:
    """Keeps the user's own preferences out of a measurement.

    Builds that read SIDESCOPES_PREFS_FILE never touch the real file at all.
    Older ones do, so it is copied aside before the first launch and put back
    afterwards however the run ends - a measured launch would otherwise leave
    the harness's window placement and scope stack behind it.
    """

    def __init__(self, scratch):
        self.override = scratch / "preferences.txt"
        self._backup = scratch / "preferences.user.txt"
        self._existed = PREFERENCES.exists()
        if self._existed and session.HARNESS_MARKER in PREFERENCES.read_text(errors="replace"):
            # A previous run was killed before it could put the file back. The
            # backup beside this one still holds the user's copy; keeping it is
            # the difference between restoring their file and overwriting it
            # with the harness's own leavings.
            self._existed = self._backup.exists()
        elif self._existed:
            shutil.copy2(PREFERENCES, self._backup)

    def write(self, text):
        for path in (self.override, PREFERENCES):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def restore(self):
        if self._existed:
            shutil.copy2(self._backup, PREFERENCES)
        elif PREFERENCES.exists():
            PREFERENCES.unlink()


def _environment(guard, diagnostics_path):
    environment = {"SIDESCOPES_PREFS_FILE": str(guard.override)}
    if diagnostics_path is not None:
        environment["SIDESCOPES_DIAG"] = "perf"
        environment["SIDESCOPES_DIAG_FILE"] = str(diagnostics_path)

    return environment


def _run_one(scenario, stack, bundle, plan, guard, helper, content_set, diagnostics_path, seconds):
    """Drive one scenario once and return its result."""
    warnings = []
    guard.write(session.preferences_text(stack, plan.application_rect))
    window = None
    application = None
    tail = session.DiagnosticTail(diagnostics_path) if diagnostics_path else None
    try:
        if scenario.content is not None:
            window = content_module.ContentWindow(helper, plan.content_rect, content_set, mode=scenario.content,
                                                  fps=scenario.content_fps)
            warnings.extend(_motion_complaints(scenario, window))
        region = plan.region_in(window.rect if window else plan.content_rect)
        # Park the pointer clear of both windows, so that a scenario which does
        # not move it starts from the same place every time.
        quartz.move_pointer((plan.content_rect[0] - 40.0, plan.content_rect[1] - 60.0))
        application = session.launch(bundle, _environment(guard, diagnostics_path))
        content_rect = window.rect if window else plan.content_rect
        if not scenario.from_launch:
            if not session.await_window(application, plan.application_rect[2:]):
                warnings.append("the application window never appeared at its expected size")
            time.sleep(session.SETTLE_SECONDS)
            if not session.establish_region(application, scenario.region, region, content_rect, BINDINGS, bundle):
                warnings.append(f"no region border appeared, so this measures an application with no "
                                f"{scenario.region} region rather than the scenario asked for")
        target = session.Target(application, bundle, BINDINGS)
        action = session.action_for(scenario.action, region, content_rect, target)
        action.start()
        try:
            if not scenario.from_launch:
                time.sleep(1.0)
            measurement = session.measure(application, window.pid if window else None, seconds, tail, action=action)
        finally:
            action.stop()
        if scenario.expects_analysis and tail is not None and measurement.passes_per_second == 0.0:
            warnings.append("nothing was analysed although the content was changing, so the screen recording "
                            "permission may be missing")
        warnings.extend(action.complaints())

        return session.ScenarioResult(scenario, stack, measurement, warnings=warnings)
    finally:
        # Each teardown step guards its own failure: an exception raised in a
        # finally block abandons the rest of it, and the step left undone was
        # the one that takes the content window off the user's screen.
        for teardown in (lambda: session.quit_application(application) if application else None,
                         lambda: window.stop() if window else None):
            try:
                teardown()
            except Exception as failure:  # noqa: BLE001 - cleanup must not mask the original failure
                print(f"    cleanup: {failure}", file=sys.stderr)


def _motion_complaints(scenario, window):
    """Why this run's content is not moving the way the scenario says it does.

    The video scenario's whole claim is that every captured frame differs from
    the one before, which is what makes it the case where nothing the
    application skips can be skipped. A pan of under a pixel a frame would leave
    frames identical and the application would rightly skip them - and the run
    would report the cost of watching a still picture as the cost of watching
    footage. So the window states what it is actually doing and this checks it,
    rather than the constants being trusted from the source.
    """
    if scenario.content != "video":
        return []
    if window.pan is None:
        return ["the content window did not report a pan, so this build of it cannot play video"]
    print(f"    content pans {window.pan['pixels_per_frame']:.0f} px/frame over "
          f"{window.pan['travel_pixels']:.0f} px at {window.pan['frames_per_second']:.0f}/s")
    if window.pan["pixels_per_frame"] < 1.0 or window.pan["travel_pixels"] < 1.0:
        return [f"the content pans {window.pan['pixels_per_frame']:.2f} pixels a frame over "
                f"{window.pan['travel_pixels']:.0f}, so some frames are identical and this is not video"]

    return []


def _rows(result, build):
    """One measurement as the flat records the comparison tool reads.

    A scenario that warned did not run as specified, so its numbers are marked
    uncomparable too: a region that failed to take costs almost nothing, which
    would read as a spectacular improvement rather than as the flaw it is.
    """
    measurement = result.measurement
    comparable = result.scenario.comparable and not result.warnings
    common = {
        "machine": build["machine"],
        "os": build["os"],
        "build": build["build"],
        "version": build["version"],
        "scenario": result.scenario.id,
        "stack": result.stack,
        "comparable": comparable,
    }
    if not comparable:
        common["incomparable_reason"] = ("; ".join(result.warnings) if result.warnings
                                         else result.scenario.incomparable_reason)
    name = f"{result.scenario.id}/{result.stack}"
    readings = (
        ("cpu", measurement.cores, "cores", "lower", "processor time over wall time, in cores of one core"),
        ("footprint", measurement.footprint_mb, "MB", "lower", "peak phys_footprint, what Activity Monitor shows"),
        ("resident", measurement.resident_mb, "MB", "lower", "peak resident size, which is NOT the footprint"),
        ("frames", measurement.frames_per_second, "per second", "none", "rendered frames, reported without a verdict"),
        ("passes", measurement.passes_per_second, "per second", "none", "analysis passes, reported without a verdict"),
        ("content-cpu", measurement.content_cores, "cores", "none",
         "what the harness's own content window cost, so two runs can be checked for equal load"),
    ) + _tracking_readings(measurement.tracking)

    return [dict(common, metric=f"{kind} {name}", value=round(value, 4), unit=unit, direction=direction, detail=detail)
            for kind, value, unit, direction, detail in readings]


def _tracking_readings(tracking):
    """How closely the border followed the pointer, for the scenarios that drag it.

    Its own function because these are the only readings with a verdict that is
    not about cost: a border that trails the hand is a defect however cheap it
    was, and this is the number that says so.
    """
    if tracking is None or not tracking.samples:
        return ()

    return (
        ("track", tracking.median_ms, "ms", "lower", "median lag of the region border behind the pointer"),
        ("track-worst", tracking.worst_ms, "ms", "lower", "worst lag of the region border behind the pointer"),
        ("track-settle", tracking.settle_ms, "ms", "lower",
         "from the release of a flick until the border stops where the pointer left it"),
    )


def _complaint_about(bundle, executable):
    """Why this run must not start, or an empty string if it may.

    All of it is about not measuring the wrong thing: an application somebody
    else launched, one nobody is actually driving, or one whose behaviour the
    harness cannot identify.
    """
    if not executable.exists():
        return f"{bundle} is not a built application bundle"
    running = session.find_any_running()
    if running:
        return ("SideScopes is already running; quit it first so that the harness measures only the launches it "
                f"made itself (pids {running})")
    if not quartz.pointer_works():
        return ("synthesised pointer events are being dropped. Grant Accessibility to the application running "
                "this script in System Settings > Privacy & Security > Accessibility.")
    if catalog.detect_profile(executable) is None:
        return "cannot tell what this build does; it matches no known behaviour profile"

    return ""


def _chosen_scenarios(names):
    """The named scenarios in the order given, or every one; None if a name is unknown."""
    chosen = [name.strip() for name in names.split(",") if name.strip()]
    if not chosen:
        return list(catalog.SCENARIOS)
    found = [catalog.scenario_named(name) for name in chosen]

    return None if any(scenario is None for scenario in found) else found


def _measure_all(scenarios, stacks, setup):
    """Drive every scenario against every stack; return rows, absences, warnings."""
    results, absent, warnings = [], [], []
    total = len(scenarios) * len(stacks)
    done = 0
    for stack in stacks:
        for scenario in scenarios:
            done += 1
            reason = catalog.unavailable(scenario, stack, setup["profile"], setup["scopes"])
            if reason:
                absent.append({"scenario": scenario.id, "stack": stack, "reason": reason})
                print(f"[{done}/{total}] {scenario.id}/{stack}: absent - {reason}")
                continue
            seconds = setup["seconds"] or scenario.seconds
            print(f"[{done}/{total}] {scenario.id}/{stack}: measuring {seconds:.0f} s", flush=True)
            result = _run_one(scenario, stack, setup["bundle"], setup["plan"], setup["guard"], setup["helper"],
                              setup["content"], setup["diagnostics"], seconds)
            results.extend(_rows(result, setup["build"]))
            for warning in result.warnings:
                warnings.append(f"{scenario.id}/{stack}: {warning}")
                print(f"    warning: {warning}")
            measurement = result.measurement
            tracked = measurement.tracking
            print(f"    {measurement.cores:.3f} cores, {measurement.footprint_mb:.0f} MB, "
                  f"{measurement.frames_per_second:.1f} frames/s, {measurement.passes_per_second:.1f} passes/s"
                  + (f", border {tracked.median_ms:.0f} ms behind the pointer "
                     f"({tracked.worst_ms:.0f} worst, settles in {tracked.settle_ms:.0f})"
                     if tracked is not None and tracked.samples else ""))

    return (results, absent, warnings)


def _destination_for(chosen, build):
    if chosen is not None:
        return chosen
    results_dir = REPOSITORY / "bench-results"
    results_dir.mkdir(parents=True, exist_ok=True)

    return results_dir / f"scenarios-{build['machine']}-{build['build'] or 'unknown'}.json"


def main(argv=None):
    arguments = _parse_arguments(sys.argv[1:] if argv is None else argv)
    if arguments.list:
        _list_catalogue()

        return 0

    bundle = arguments.app.resolve()
    executable = bundle / "Contents" / "MacOS" / "SideScopes"
    complaint = _complaint_about(bundle, executable)
    if complaint:
        print(f"app-scenarios: {complaint}", file=sys.stderr)

        return 2
    scenarios = _chosen_scenarios(arguments.scenarios)
    if scenarios is None:
        print(f"app-scenarios: unknown scenario in {arguments.scenarios!r}", file=sys.stderr)

        return 2
    stacks = [stack.strip().upper() for stack in arguments.stacks.split(",") if stack.strip()]

    cache = content_module.cache_directory()
    scratch = cache / "run"
    scratch.mkdir(parents=True, exist_ok=True)
    content_set = content_module.prepare(cache / "images", arguments.photographs)
    display = _target_display(arguments.display)
    plan = catalog.Layout(display, tuple(int(part) for part in arguments.region_pixels.lower().split("x")))
    facts = conditions.collect(bundle, display, content_set.describe())
    diagnostics_path = None if arguments.no_diagnostics else scratch / "diagnostics.log"
    if diagnostics_path is not None:
        diagnostics_path.unlink(missing_ok=True)

    profile = catalog.detect_profile(executable)
    setup = {
        "bundle": bundle,
        "plan": plan,
        "profile": profile,
        "scopes": catalog.detect_scopes(executable),
        "guard": PreferencesGuard(scratch),
        "helper": content_module.build_helper(cache),
        "content": content_set,
        "diagnostics": diagnostics_path,
        "seconds": arguments.seconds,
        "build": {"machine": facts["machine"]["name"], "os": facts["machine"]["os"],
                  "build": facts["application"]["binary_sha256"], "version": facts["application"]["version"]},
    }
    try:
        results, absent, warnings = _measure_all(scenarios, stacks, setup)
    finally:
        setup["guard"].restore()
    if content_set.degraded:
        warnings.insert(0, content_set.reason)

    document = {
        "schema": "sidescopes-app-scenarios/1",
        "conditions": facts,
        "layout": plan.describe(),
        "profile": {"name": profile.name, "behaviour": profile.summary, "scopes": setup["scopes"],
                    "honours_prefs_override": b"SIDESCOPES_PREFS_FILE" in catalog.strings_in(executable)},
        "results": results,
        "absent": absent,
        "warnings": warnings,
    }
    destination = _destination_for(arguments.out, setup['build'])
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(document, indent=2) + "\n")
    print(destination)

    return 0


if __name__ == "__main__":
    sys.exit(main())
