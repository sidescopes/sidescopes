"""What the measured application is pointed at, and the window that shows it.

The default content is SYNTHETIC and always sufficient. A generated image is the
better instrument for regression work: identical on every machine and every run,
immune to a URL changing under it, no licence to honour, and it works offline.
The patterns deliberately span the range the engines care about - large flat
fields and hard edges at one end, full-amplitude noise at the other - because
flat and detailed content cost very different amounts.

Photographs are an opt-in layer for realism and for judging a scope by eye. They
are fetched on demand into a cache that is never committed, and every download
is checked against a digest recorded in the manifest beside this file: an image
that silently changed would silently change the measurements, which is the worst
way a benchmark can fail. If a fetch fails, the run degrades to synthetic
content and says so in its output, so a run with different inputs can never be
mistaken for one without.
"""

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

MANIFEST = pathlib.Path(__file__).with_name("photos.json")
HELPER_SOURCE = pathlib.Path(__file__).with_name("content_window.m")

# The patterns a run uses when nothing else is asked for. Ordered so that a
# switching scenario alternates between the cheap and the expensive end.
DEFAULT_PATTERNS = ("photoish", "bars", "skin", "noise")

_FETCH_TIMEOUT_SECONDS = 30
_FETCH_ATTEMPTS = 4


class ContentSet:
    """The images a run showed, and whether they are the ones it asked for."""

    def __init__(self, kind, patterns=(), files=(), digests=(), degraded=False, reason=""):
        self.kind = kind
        self.patterns = tuple(patterns)
        self.files = tuple(files)
        self.digests = tuple(digests)
        self.degraded = degraded
        self.reason = reason

    def describe(self):
        """What a later reader needs to know a comparison used the same inputs."""
        return {
            "kind": self.kind,
            "patterns": list(self.patterns),
            "photographs": [{"name": pathlib.Path(path).name, "sha256": digest[:12]}
                            for path, digest in zip(self.files, self.digests)],
            "degraded": self.degraded,
            "reason": self.reason,
        }


def _digest_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)

    return digest.hexdigest()


def _download(url, destination):
    """Fetch one file, retrying the rate limit the image host applies."""
    request = urllib.request.Request(url, headers={"User-Agent": "sidescopes-scenarios"})
    for attempt in range(_FETCH_ATTEMPTS):
        try:
            with urllib.request.urlopen(request, timeout=_FETCH_TIMEOUT_SECONDS) as response, \
                    open(destination, "wb") as handle:
                shutil.copyfileobj(response, handle)

            return
        except urllib.error.HTTPError as failure:
            if failure.code != 429 or attempt == _FETCH_ATTEMPTS - 1:
                raise
            time.sleep(4.0 * (attempt + 1))


def _fetch(entry, cache_dir):
    """The cached path for one manifest entry, downloading it if need be.

    A digest that does not match is treated exactly like a missing file: the
    bytes are thrown away and the caller degrades. Serving a different image
    than the manifest describes would quietly invalidate every comparison the
    file appears in.
    """
    target = cache_dir / entry["name"]
    if target.exists() and _digest_of(target) == entry["sha256"]:
        return (target, entry["sha256"])
    partial = target.with_suffix(target.suffix + ".part")
    _download(entry["url"], partial)
    actual = _digest_of(partial)
    if actual != entry["sha256"]:
        partial.unlink(missing_ok=True)
        raise ValueError(f"{entry['name']} hashes to {actual[:12]}, the manifest says {entry['sha256'][:12]}")
    partial.replace(target)

    return (target, actual)


def photographs(cache_dir):
    """Every manifest photograph, fetched if the cache does not hold it yet."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    entries = json.loads(MANIFEST.read_text()).get("images", [])
    if not entries:
        raise ValueError("the photograph manifest names no images")

    return [_fetch(entry, cache_dir) for entry in entries]


def prepare(cache_dir, want_photographs, patterns=DEFAULT_PATTERNS):
    """Decide what a run will show, degrading to synthetic rather than failing."""
    if not want_photographs:
        return ContentSet("synthetic", patterns=patterns)
    try:
        fetched = photographs(cache_dir)
    except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as failure:
        return ContentSet("synthetic", patterns=patterns, degraded=True,
                          reason=f"photographs unavailable ({failure}); measured against generated content instead")

    return ContentSet("photographs", files=[path for path, _ in fetched], digests=[digest for _, digest in fetched])


def build_helper(cache_dir):
    """Compile the content window, reusing the binary until its source changes.

    Compiling on demand keeps the harness a set of scripts that runs against any
    build of the application, including a released one from a tag, without
    entering the project's own build.
    """
    cache_dir.mkdir(parents=True, exist_ok=True)
    binary = cache_dir / "content_window"
    if binary.exists() and binary.stat().st_mtime >= HELPER_SOURCE.stat().st_mtime:
        return binary
    compiler = shutil.which("cc")
    if compiler is None:
        raise RuntimeError("no C compiler found; install the Xcode command line tools")
    finished = subprocess.run(
        [compiler, "-fobjc-arc", "-O2", "-Wall", "-Wextra", "-framework", "Cocoa", "-framework", "QuartzCore",
         "-o", str(binary), str(HELPER_SOURCE)],
        capture_output=True, text=True, check=False)
    if finished.returncode != 0:
        raise RuntimeError(f"cannot build the content window:\n{finished.stderr}")

    return binary


class ContentWindow:
    """The generated content on screen, as a process the harness owns.

    The window reports the rectangle it actually achieved - the system moves a
    window whose title bar would fall off the screen - and callers must aim the
    region at that rather than at what they asked for.
    """

    def __init__(self, binary, rect, content_set, mode="still", period=2.0, fps=None):
        arguments = [str(binary), "--rect", ",".join(f"{value:.0f}" for value in rect), "--mode", mode,
                     "--period", str(period)]
        if fps is not None:
            arguments += ["--fps", str(fps)]
        if content_set.files:
            arguments += ["--image", ",".join(str(path) for path in content_set.files)]
        else:
            arguments += ["--pattern", ",".join(content_set.patterns)]
        self._process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.rect = rect
        self.pid = self._process.pid
        # How far the video mode moves the content between frames, and over what
        # travel, straight from the window that does the moving. A pan of under
        # a pixel a frame would leave frames the application is entitled to skip,
        # which is the one thing a video measurement must not contain.
        self.pan = None
        self._read_header()

    def _read_header(self):
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            line = self._process.stdout.readline()
            if line == "":
                raise RuntimeError(f"the content window exited: {self._process.stderr.read()}")
            line = line.strip()
            if line.startswith("content_rect "):
                self.rect = tuple(float(part) for part in line.split(" ", 1)[1].split(","))
            elif line.startswith("pan "):
                step, travel, fps = (float(part) for part in line.split(" ", 1)[1].split(","))
                self.pan = {"pixels_per_frame": step, "travel_pixels": travel, "frames_per_second": fps}
            elif line == "ready":
                return
        raise RuntimeError("the content window never reported itself ready")

    def centre(self):
        return (self.rect[0] + (self.rect[2] / 2.0), self.rect[1] + (self.rect[3] / 2.0))

    def inset(self, margin):
        """The content rectangle pulled in on every side, as (x, y, w, h)."""
        return (self.rect[0] + margin, self.rect[1] + margin,
                max(self.rect[2] - (2 * margin), 1.0), max(self.rect[3] - (2 * margin), 1.0))

    def stop(self):
        if self._process.poll() is None:
            self._process.terminate()
            try:
                self._process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._process.kill()
        for stream in (self._process.stdout, self._process.stderr):
            if stream is not None:
                stream.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop()
        return False


def cache_directory():
    """Where downloads, the compiled helper and a run's scratch files live.

    Outside every repository by default, which settles the question of what a
    checkout must ignore: nothing. An environment variable moves it, for a
    build agent that would rather keep its caches together.
    """
    override = os.environ.get("SIDESCOPES_SCENARIO_CACHE")
    if override:
        return pathlib.Path(override)
    if sys.platform == "win32":
        base = pathlib.Path(os.environ.get("LOCALAPPDATA", pathlib.Path.home()))

        return base / "sidescopes" / "scenarios"

    return pathlib.Path.home() / ".cache" / "sidescopes" / "scenarios"


def _fetch_named(names, cache_dir):
    """Fetches the named manifest entries into the cache. Prints one line per
    photograph so a build log says what it got and what it did not."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    entries = {entry["name"]: entry for entry in json.loads(MANIFEST.read_text()).get("images", [])}
    missing = [name for name in names if name not in entries]
    for name in missing:
        print(f"content: {name} is not in the manifest", file=sys.stderr)
    got = 0
    for name in names:
        entry = entries.get(name)
        if entry is None:
            continue
        try:
            path, _ = _fetch(entry, cache_dir)
        except (OSError, ValueError, urllib.error.URLError) as failure:
            # Degrading is the contract the whole manifest is written around:
            # the caller falls back and says so, rather than measuring or
            # showing something it cannot name.
            print(f"content: {name} unavailable - {failure}", file=sys.stderr)
            continue
        print(f"content: {path}")
        got += 1

    return 0 if got == len(names) and not missing else 1


if __name__ == "__main__":
    # `python3 content.py <cache-dir> <name>...` - the lab build's way of
    # getting the photographs it shows without depending on anyone having run
    # the scenario harness first.
    raise SystemExit(_fetch_named(sys.argv[2:], pathlib.Path(sys.argv[1])))
