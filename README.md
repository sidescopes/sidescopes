# SideScopes

Real-time color scopes for any region of your screen. SideScopes shows a
live vectorscope, waveform and histogram in a small window beside whatever
you are working in.

It reads the screen rather than your files, so it asks nothing of the
application you are looking at — no plugin, no export step, no list of
supported programs. That is why it works with all of them.

## What you see

A compact window that stays above the others, sized for the corner of a
laptop screen. Across the top is a toolbar: a button that chooses which
scopes are shown, a button for saved layouts, and the region tools — draw a
region, attach to a window, attach to a face, clear the region. Under it
are the scopes themselves, one pane each, splitting the window as you turn
more of them on. Along the bottom sits the color under your pointer with
its RGB percentages, and the tool that pins a color for later comparison.

There are six scopes, and any combination of them can be on screen at once:
a vectorscope, an RGB waveform, a luma waveform, an RGB parade, a
per-channel histogram and a combined one. A color picker pane joins them,
holding the live color against the ones you have pinned.

The region being measured carries a live border on the desktop, labeled
with the window it belongs to. Drag the border to move the region, drag a
handle to resize it, and the scopes follow as you go.

Letters switch scopes — V for the vectorscope, W for the waveform — and
holding Shift adds one alongside the others instead of replacing them.
Scrolling over a trace adjusts how brightly it is drawn. Everything else is
on the right-click menu; there is no menu bar to learn.

## Who it is for

If your editor already shows a vectorscope and a waveform, you do not need
this. Video grading tools have had excellent ones for decades, built right
into the application. SideScopes is for everyone else: people whose tool
offers a histogram and little more, or nothing at all.

That is most photo editors, Lightroom, Capture One and darktable among
them. It is also design and layout tools, image viewers, and a browser
showing a page whose colors you are checking. The scopes do not care which
application drew the pixels.

Photography is where SideScopes was built and where its defaults were
calibrated, so that is the work it fits most closely.

## What it answers

- Is this white balance actually neutral? A neutral frame sits on the
  center of the vectorscope. A cast pushes the whole cloud off center, and
  the direction it goes names the cast.
- Does this skin tone sit where skin tones sit? The vectorscope's skin-tone
  line is the reference, and the marker following your pointer shows where
  the tone you are hovering lands against it.
- Are the highlights clipping, and which channel goes first? The top of the
  waveform says whether, and the RGB parade separates the channels to say
  which — red clipping in a sunset is visible there well before the image
  looks wrong.
- Is the exposure even across the frame? The luma waveform reads left to
  right across the region, so a bright corner or a fall-off is a slope in
  the trace.
- How far does this sky stretch toward cyan? Distance from the center of
  the vectorscope is saturation, and the graticule targets say how far is
  far.
- Do these two images match? Pin a color from the first and hover the
  second: the color picker holds them side by side and gives the difference
  as ΔE, split into lightness, chroma and hue.

## Install

Download the build for macOS or Windows from the
[releases page](https://github.com/sidescopes/sidescopes/releases), unzip
it, and run it. The Windows build is a single executable and needs no
runtime installed.

macOS asks for the Screen Recording permission the first time. SideScopes
cannot read the screen without it, and it asks for nothing else.

To build from source, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

- [The scopes](docs/SCOPES.md) — what each one measures, how to read it,
  and the controls that shape a trace
- [Choosing a region](docs/REGIONS.md) — the region tools, the border on
  the desktop, and what attaching to a window does
- [Keyboard and mouse](docs/SHORTCUTS.md) — the complete reference
- [Troubleshooting](docs/TROUBLESHOOTING.md) — permissions, interrupted
  capture, and diagnostic logs
- [Design](DESIGN.md) — the architecture and the reasoning behind it
- [Scope modules](docs/MODULES.md) — the C ABI every scope loads through
- [Contributing](CONTRIBUTING.md) — toolchain setup, the checks CI runs,
  and what a change is expected to ship with

## Status

Native builds for macOS and Windows; Linux is planned. Before 1.0 the
preferences file may change shape without a migration, so an update can
return settings to their defaults.

## License

SideScopes is free and open-source software, licensed under the
[GNU GPL v3.0 or later](LICENSE).

Bundled components: Dear ImGui (MIT), GLFW (zlib), Catch2 (BSL-1.0),
nlohmann/json (MIT), NanoSVG (zlib). Icons from [Lucide](https://lucide.dev)
(ISC).
