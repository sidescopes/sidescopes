# SideScopes

SideScopes is a free, open-source set of color scopes for any selected region
of the screen. It provides a vectorscope, RGB waveform, luma waveform, RGB
parade, histogram, and color comparison in a compact always-on-top window.

Because SideScopes analyzes the rendered screen rather than opening source
files, it can work beside an image editor, video application, compositor,
browser, reference viewer, or other visible content. It does not require a
plugin or a supported-application list.

## Application

Select a rectangle anywhere on screen, attach a region to a window, or use face
selection where the platform supports it. The desktop border can move and
resize the active region while the scopes update.

Any combination of instruments can share the application window. Layouts can
be reordered, divided horizontally or vertically, and saved in nine preset
slots. Keyboard shortcuts cover scope selection, region tools, pinning, and
presets; the right-click menu contains instrument and application settings.

The pointer readout reports captured RGB values. Click to pin a pixel or drag
to pin an area average, then compare it with another sample using assumed-sRGB
CIELAB values and CIEDE2000 color difference.

SideScopes measures the composited display output available to screen capture.
It does not read the source document profile, timeline color space, RAW data,
display ICC profile, or HDR metadata. See [The scopes](docs/SCOPES.md) for the
interpretation and measurement boundary of each instrument.

## SideScopes Lab

[SideScopes Lab](https://sidescopes.org/lab/) runs the shared C++ scope engines
in a browser. It includes sample images, local image loading, movable analysis
regions, and image adjustments for observing how the traces respond. Files
loaded into the Lab remain in the browser and are not uploaded.

The Lab analyzes still images within its page. Selecting desktop windows,
following screen regions, and live capture from other applications require the
desktop application.

## Install

Download the current macOS or Windows build from the
[releases page](https://github.com/sidescopes/sidescopes/releases). Both builds
are portable zip archives.

- **macOS 14 or later**, Apple silicon or Intel. Screen capture requires the
  Screen Recording permission.
- **Windows 10 or later**, 64-bit. Extract the archive and run the executable;
  no separate runtime is required.

Current releases are not code-signed. The
[download page](https://sidescopes.org/download/) explains the operating-system
warnings and how to verify a release checksum.

To build from source, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

- [The scopes](docs/SCOPES.md) — measurements, interpretation, controls, and
  color assumptions
- [Choosing a region](docs/REGIONS.md) — drawing, attaching, face selection,
  and the desktop border
- [Keyboard and mouse](docs/SHORTCUTS.md) — complete input reference
- [Troubleshooting](docs/TROUBLESHOOTING.md) — permissions, capture recovery,
  preferences, and diagnostic logs
- [Design](DESIGN.md) — application architecture and design constraints
- [Scope modules](docs/MODULES.md) — the scope module C ABI
- [Contributing](CONTRIBUTING.md) — toolchains, tests, and contribution process

The educational [scope guides](https://sidescopes.org/learn/) explain the
instruments independently of the application.

## Status

Native builds are available for macOS and Windows. Linux is not currently
released. Before 1.0, an update may reset preferences when their format
changes.

## Privacy

The desktop application has no network access, telemetry, account, or update
service. Captured pixels remain on the computer. It writes a preferences file
and, only when requested, a local diagnostic log.

## License

SideScopes is free software licensed under the
[GNU GPL v3.0 or later](LICENSE). The SideScopes name and mark are not covered
by that license and may not identify a modified version; see
[assets/brand](assets/brand).

Bundled components include Dear ImGui (MIT), GLFW (zlib), NanoSVG (zlib),
and Lucide icons (ISC). The Lab also includes Inter and Roboto Mono (OFL).
Distributions carry the applicable notices; see [licenses](licenses).
Catch2 (BSL-1.0) is used for tests and benchmarks.
