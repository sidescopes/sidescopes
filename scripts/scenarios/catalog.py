"""What the harness measures, and what it refuses to compare.

The application's behaviour changed between releases, so two builds are not
always doing the same thing when they are asked the same question. The version
string cannot be used to tell them apart - it only moves at a release, so a tag
and the development cycle after it report the same one - and neither can a date.
What identifies a build here is the FEATURES its binary carries, probed from the
strings it was compiled with.

Two consequences run through this file. A scenario that needs something a build
does not have is reported ABSENT with the reason, never quietly replaced by
something similar. And a scenario that exists in both builds but means different
things in them - the state a fresh launch settles into, most of all - is
reported as NOT COMPARABLE, so that a behaviour change can never be read as a
speed-up.
"""

# Scope stack letters, and the identifier whose presence in a binary proves the
# build has that scope. The colour picker is the host's own pane rather than a
# module, but it is named in the same reverse-DNS style and persisted the same
# way, so the same probe finds it.
SCOPE_LETTERS = {
    "V": "org.sidescopes.vectorscope",
    "W": "org.sidescopes.waveform",
    "L": "org.sidescopes.waveform.luma",
    "R": "org.sidescopes.parade",
    "H": "org.sidescopes.histogram",
    "C": "org.sidescopes.colorpicker",
}

# The stacks a run measures unless told otherwise: one scope, the owner's
# working set, and every scope that has existed since 0.4. "Every scope" is
# deliberately not spelled as "all" - a build with a scope the other lacks would
# make that phrase mean two different things, and the luma waveform is exactly
# such a scope. Ask for it by name.
DEFAULT_STACKS = ("W", "WVR", "VWRHC")

# The rate the video scenario plays at. Cinema's frame rate rather than the
# display's: the case being priced is a colourist watching footage, and what the
# application costs there is set by the fact that every frame it captures is a
# different one - not by how fast the source runs, as long as it runs faster
# than the capture cadence.
VIDEO_FRAMES_PER_SECOND = 24.0


class Profile:
    """A build's behaviour, as far as driving it is concerned."""

    def __init__(self, name, marker, capabilities, summary):
        self.name = name
        self.marker = marker
        self.capabilities = frozenset(capabilities)
        self.summary = summary


# Probed in order; the first marker found in the binary wins.
PROFILES = (
    Profile(
        "region-optional",
        "shortcut_clear_region",
        {"start-empty", "clear-region", "draw-region", "attach-window"},
        "starts with no region and analyses nothing until one is chosen; Escape clears the region",
    ),
    Profile(
        "always-scoping",
        "shortcut_full_screen",
        {"draw-region", "attach-window"},
        "starts on a whole-display region and is always analysing; Escape watches the whole display",
    ),
)


class Scenario:
    """One measurable situation.

    @param content     None for an empty screen, else the mode the content
                       window runs in: still, switch or animate.
    @param region      none, draw or attach.
    @param action      What the harness does during the measurement.
    @param needs       Capabilities a build must have for this to be run.
    @param comparable  False when both builds can run it but the results do not
                       mean the same thing.
    """

    def __init__(self, identifier, summary, content, region, action, seconds=15.0, needs=(), comparable=True,
                 incomparable_reason="", from_launch=False, content_fps=None):
        self.id = identifier
        self.summary = summary
        self.content = content
        self.region = region
        self.action = action
        # How often the content window advances, for the modes where the rate is
        # part of what the scenario means. Video is watched at a frame rate, and
        # a scenario that did not name one would be measuring an arbitrary one.
        self.content_fps = content_fps
        self.seconds = seconds
        self.needs = frozenset(needs)
        self.comparable = comparable
        self.incomparable_reason = incomparable_reason
        # Measure from the moment the process exists rather than from a settled
        # state, for the one scenario that is about starting up.
        self.from_launch = from_launch

    @property
    def expects_analysis(self):
        """Whether the PIXELS under the region change during this scenario.

        A region over content that does not change runs no analysis passes at
        all, by design - the change detector finds nothing to do - so a pass
        count of zero is a failure signal only where something really moves
        under the region.

        Dragging an ATTACHED window is not such a case, and measured so: the
        region travels with the window, so it keeps seeing the same pixels and
        the detector is right to skip them. What that scenario costs is the
        tracking, not the analysis.
        """
        return self.content in ("switch", "animate", "video") or self.action in ("region-drag", "region-flick")


SCENARIOS = (
    Scenario(
        "idle-no-region", "nothing selected, nothing moving", None, "none", "still",
        needs=("start-empty",),
    ),
    Scenario(
        "idle-region", "a region over content that does not change", "still", "draw", "still",
    ),
    Scenario(
        "pointer-over-region", "the pointer sweeping across varied content inside the region",
        "still", "draw", "pointer-sweep",
    ),
    Scenario(
        "content-switch", "the picture under a static region changing every two seconds",
        "switch", "draw", "still",
    ),
    Scenario(
        "content-animate", "content under a static region changing every frame",
        "animate", "draw", "still",
    ),
    Scenario(
        "video-watch", "a region over footage playing at 24 frames a second, the pointer parked",
        "video", "draw", "still", content_fps=VIDEO_FRAMES_PER_SECOND,
    ),
    Scenario(
        "region-scan", "the region dragged back and forth across the content",
        "still", "draw", "region-drag",
    ),
    Scenario(
        "region-flick", "the region thrown across the content in quick, rough moves",
        "still", "draw", "region-flick",
    ),
    Scenario(
        "region-redraw", "a region drawn roughly in a fifth of a second, cleared, and drawn again",
        "still", "none", "region-redraw", needs=("draw-region",),
    ),
    Scenario(
        "attached-window-drag", "attached to a window, and that window dragged about",
        "still", "attach", "window-drag", needs=("attach-window",),
    ),
    Scenario(
        "startup-default", "the first seconds after launch, in whatever state the build starts in",
        "still", "none", "still", seconds=10.0, comparable=False, from_launch=True,
        incomparable_reason="the state a fresh launch settles into differs between builds, so this measures two "
                            "different situations - a behaviour change, not a speed difference",
    ),
)


def scenario_named(identifier):
    for scenario in SCENARIOS:
        if scenario.id == identifier:
            return scenario

    return None


def strings_in(path):
    """Every printable run of bytes in a file, as one lowercase blob.

    Reading the binary is how a build is identified: what it can do is decided
    by the code in it, and unlike a version string that cannot be stale.
    """
    with open(path, "rb") as handle:
        return handle.read()


def detect_profile(executable):
    """Which behaviour profile a built application follows."""
    blob = strings_in(executable)
    for profile in PROFILES:
        if profile.marker.encode() in blob:
            return profile

    return None


def detect_scopes(executable):
    """The stack letters a built application offers."""
    blob = strings_in(executable)

    return "".join(letter for letter, identifier in SCOPE_LETTERS.items() if identifier.encode() in blob)


def unavailable(scenario, stack, profile, scopes):
    """Why this build cannot run this scenario, or an empty string if it can."""
    missing = scenario.needs - profile.capabilities
    if missing:
        return f"this build has no {', '.join(sorted(missing))} ({profile.summary})"
    absent = [letter for letter in stack if letter not in scopes]
    if absent:
        return f"this build has no scope {', '.join(absent)}"

    return ""


# --- Where things go on the screen ------------------------------------------

# The region is sized in CAPTURED PIXELS rather than points, because that is
# what the analysis actually costs: the same rectangle in points is twice the
# work on a Retina panel. Two machines compared at the same pixel count are
# doing the same amount of work.
DEFAULT_REGION_PIXELS = (1600, 1000)

_CONTENT_INSET = 40.0
_SCREEN_MARGIN = 60.0
_WINDOW_GAP = 40.0
_TITLE_BAR = 30.0
_APPLICATION_SIZE = (440.0, 720.0)


class Layout:
    """Where the content window, the application and the region sit.

    Discovered from the display at run time - a machine's display arrangement
    changes between sessions, and assuming one has produced measurements of the
    wrong thing before.
    """

    def __init__(self, display, region_pixels):
        scale = display["scale"] or 1.0
        left, top = display["origin"]
        width, height = display["points"]
        available_width = width - (2 * _SCREEN_MARGIN) - _WINDOW_GAP - _APPLICATION_SIZE[0] - (2 * _CONTENT_INSET)
        available_height = height - (2 * _SCREEN_MARGIN) - _TITLE_BAR - (2 * _CONTENT_INSET)
        wanted = (region_pixels[0] / scale, region_pixels[1] / scale)
        shrink = min(1.0, available_width / wanted[0], available_height / wanted[1])
        if shrink <= 0.0:
            raise ValueError(f"display {display['id']} is too small for a measurement region")
        self.shrunk = shrink < 1.0
        self.region_points = (wanted[0] * shrink, wanted[1] * shrink)
        self.region_pixels = (round(self.region_points[0] * scale), round(self.region_points[1] * scale))
        self.content_rect = (left + _SCREEN_MARGIN, top + _SCREEN_MARGIN + _TITLE_BAR,
                             self.region_points[0] + (2 * _CONTENT_INSET),
                             self.region_points[1] + (2 * _CONTENT_INSET))
        application_height = min(_APPLICATION_SIZE[1], height - (2 * _SCREEN_MARGIN) - _TITLE_BAR)
        self.application_rect = (left + width - _SCREEN_MARGIN - _APPLICATION_SIZE[0],
                                 top + _SCREEN_MARGIN + _TITLE_BAR, _APPLICATION_SIZE[0], application_height)

    def region_in(self, content_rect):
        """The region rectangle inside a content window's achieved rectangle."""
        return (content_rect[0] + _CONTENT_INSET, content_rect[1] + _CONTENT_INSET,
                self.region_points[0], self.region_points[1])

    def describe(self):
        return {
            "region_points": [round(value) for value in self.region_points],
            "region_pixels": list(self.region_pixels),
            "region_shrunk_to_fit": self.shrunk,
            "content_rect": [round(value) for value in self.content_rect],
            "application_rect": [round(value) for value in self.application_rect],
        }
