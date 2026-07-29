# SideScopes

Real-time color scopes for any region of your screen. SideScopes shows a live
vectorscope and waveform beside any editor — Lightroom, Capture One, darktable,
Resolve, or anything else that draws pixels.

Most editors ship a histogram and little else. Colorists have relied on
vectorscopes and waveforms for decades because they answer questions a
histogram cannot: is the white balance actually neutral, how far does the
sky stretch toward cyan, do the highlights clip past the level you care
about. SideScopes brings those instruments to any editor, reading the
screen itself, so it works with all of them and needs no plugins.

## Features

- Vectorscope (BT.709) with classic graticule and skin-tone line
- Waveform with RGB, luma, and colored-luma styles, plus an RGB parade
- Histogram with per-channel or combined bars
- Color picker that holds the live color against pinned references, with
  values you can copy at a click
- Smart region selection: click a window or a detected face, or draw the region
  yourself — the screenshot-tool idiom you already know
- Live marker showing the color under your cursor on every scope, with
  pinnable reference colors for matching tones across images
- Compact, always-on-top window designed for small laptop screens
- Native context menus and direct manipulation; scroll to adjust trace intensity

## Keyboard

| Key | Action |
| --- | ------ |
| V / W / R / H / C | show one scope alone: vectorscope, waveform, RGB parade, histogram, color picker |
| Shift+V / W / R / H / C | stack or unstack a scope; panes follow the order they were turned on |
| A | attach to a window |
| F | attach to a face |
| D | draw a region |
| Esc | clear the region; the scopes go empty |
| P | pin the color under the cursor |

The region border is live on the desktop: drag it to move the region,
drag a corner handle to resize, and drag an edge's midpoint handle to
resize that edge alone.

## Status

First public release. Native builds for macOS and Windows; Linux is planned.
See the releases page for downloads.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

Requires CMake 3.24+ and a C++20 compiler. Dependencies are fetched during
configuration. Toolchain setup for macOS (Homebrew) and Windows (winget) is
covered in [CONTRIBUTING.md](CONTRIBUTING.md).

## Troubleshooting

**The permission loop.** macOS ties the Screen Recording permission to the
app's code signature. If SideScopes keeps asking for permission although
System Settings shows it granted - typical after replacing the app with a
differently signed build - clear the stale grant and approve once more:

```sh
tccutil reset ScreenCapture org.sidescopes.app
```

Then relaunch SideScopes and grant the permission when prompted. Toggling
the switch in System Settings only takes effect after the app restarts.

**The Diagnostics menu.** Right-click and open Diagnostics when reporting
a problem. "Record Diagnostic Log" streams timestamped state lines to
`sidescopes-diag.log` in a `sidescopes` folder inside the system
temporary directory while you reproduce the issue - "Show Diagnostic
Log" opens that folder; attach the log to the report. It records window
titles and application names, so glance over it before sharing. On
Windows, "Show in Screen Captures" makes the SideScopes windows visible
to screenshots, which they normally are not so the scopes never analyze
themselves. "Reset to Defaults" returns everything to the standard state;
so does a restart.

For development use, `SIDESCOPES_DIAG=attach,border` (or `all`) starts
recording from launch with only the named channels (`attach`:
window-focus routing, `border`: region-border drawing, `suggestions`:
the picker's window suggestions and pick mapping, `facelock`: face-lock
probe verdicts, `perf`: frame, analysis-pass, and capture-cadence
timings); the previous run is kept beside the log as
`sidescopes-diag.prev.log`. `SIDESCOPES_DIAG_FILE` overrides the
location. Writes reach the disk on a short interval by default;
`SIDESCOPES_DIAG_FLUSH=1` flushes every line when chasing a crash, and
`SIDESCOPES_DIAG_FLUSH=0` buffers until recording stops so logging does
not distort performance measurement.

## License

SideScopes is free and open-source software, licensed under the
[GNU GPL v3.0 or later](LICENSE).

Bundled components: Dear ImGui (MIT), GLFW (zlib), Catch2 (BSL-1.0),
nlohmann/json (MIT), NanoSVG (zlib). Icons from [Lucide](https://lucide.dev)
(ISC).
