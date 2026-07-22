# Design

This document grows alongside the implementation. It records the goals, the
architecture, and the reasoning behind decisions that are not obvious from
the code.

## Goals, in priority order

1. **Useful beside a real editor.** A compact, always-on-top window showing
   one scope at a time — or a stack of them when you want several — scoped to
   a user-selected screen region. Small-laptop screens are the design target,
   not the exception.
2. **Trustworthy readings.** The scope math is unit-tested against exact
   expectations; graticule overlays are computed from the same projection the
   engines use, so they cannot disagree with the data.
3. **Fast and quiet.** Native code, analysis off the UI thread, and content
   hashing so a static screen costs next to nothing. The UI never stalls on
   frame processing.
4. **Portable core.** Everything except capture and window chrome is plain
   C++20. Platform code is confined behind narrow interfaces designed against
   the most constrained backend (Linux portals), so later ports add backends,
   not rewrites.

## Architecture

```
src/core       portable: frame types, frame handoff, scope engines
src/platform   interfaces + per-OS backends (capture, menus, region overlay)
src/app        UI shell (Dear ImGui), preferences, wiring
tests          mirrors src, one test file per module
```

Data flow: the capture backend delivers whole-display frames into a
latest-frame slot (the producer never blocks, stale frames are dropped); an
analysis thread crops to the region of interest, skips identical content via
a sparse hash, runs the scope engines, and publishes double-buffered images;
the UI thread uploads changed images to textures and draws.

Region cropping is always done app-side. Some capture APIs can crop at the
source and may be used as an optimization, but correctness never depends on
it — the least capable backend defines the contract.

## Scopes

SideScopes ships five scopes; any subset can be shown at once, splitting the
window in the order they were turned on.

- **Vectorscope** — a chroma density plot in BT.601 or BT.709, with the
  classic graticule: primary and secondary targets at 75% and 100%, and a
  skin-tone line. It magnifies 1×/2×/4× to inspect the neutral core.
- **Waveform** — luma and per-channel levels across the image width. The RGB
  overlay is the default, because separated colored traces make a color cast
  readable at a glance; a plain luma style and a colored-luma style (the luma
  trace carrying each column's average color) are also available.
- **RGB parade** — the waveform engine with the three channels laid side by
  side.
- **Histogram** — the classic distribution, per channel or combined.
- **Color picker** — not a plot but a comparison pane: the color under the
  cursor held against a pinned reference, with readouts you can copy.

The density scopes accumulate samples into integer bins and build a display
image per frame, normalized to the densest bin so sparse traces stay visible
while dominant content dominates. Trace intensity is a single 0–100% control
that is exponential in the underlying gain, so every step feels the same size;
bin counts are normalized by sample count, which keeps intensity stable when
the sampling stride changes. The histogram maps bar heights with a
square-root curve instead, the way photo editors draw it. Display images are
interpolated up from the accumulation grid rather than accumulated at display
resolution, which keeps a large pane smooth without inventing detail the data
does not contain.

Every scope exposes one projection function mapping a color to scope
coordinates. Graticule targets, the cursor marker, and pinned colors all go
through it, which is what guarantees the overlays agree with the data.

## The cursor marker

The color under the pointer is sampled from the captured frame (a small
neighborhood average), smoothed with a per-scope exponential moving average,
and drawn as a marker on every visible scope. The pipeline stays in floating
point end to end: quantizing intermediate values makes the marker visibly
dither between adjacent bins while it settles. A small snap window ends the
asymptotic tail of the average decisively.

## Region selection

The analyzed region is chosen the way a screenshot tool works. A toolbar — and
keyboard shortcuts — opens a picker over the dimmed screen: click a window to
scope it, draw an arbitrary region by hand, or, where the platform can
detect faces, click a face to scope the skin around it. Escape falls back to
the whole screen. Once confirmed, the region carries a live border on the
desktop, drawn like a macOS screenshot selection, that moves and resizes in
place without reopening the picker.

The picker suggests only exact information — real window rectangles and
detected faces — and leaves everything else to a manual draw, on the principle
that an unreliable guess is worse than an honest selection.

Reference colors can be pinned on the vectorscope and the color picker for
matching tones across photos. A dedicated pin tool turns the cursor into a
live swatch: a click pins a cursor-sized sample and a drag pins the average of
a rectangle — photographs are textured, so a useful sample comes from a patch,
not a single pixel — while holding Shift keeps pinning without closing the
tool.

## Multiple displays

The picker opens on the display under the cursor, and confirming a region
there switches capture to that display. A region is a rectangle on one
display, so it is cleared when the display changes. If a display disconnects,
capture pauses and the scopes say so; it resumes automatically when the
display returns.

## Interaction model

Three layers, no native settings window to maintain per platform:

1. Direct manipulation for continuous values — scroll over a scope to adjust
   intensity, double-click to reset, drag on screen to select the region.
   Gestures show their value transiently and always have an obvious revert.
2. A native context menu (right-click) for modes and toggles.
3. A slim ImGui panel for diagnostics and the few remaining sliders.

Scopes are toggled by letter and stacked with Shift; those shortcuts and a few
display choices live in a plain-text preferences file. Defaults matter more
than options: shipped values come from calibration against real editing
sessions, per scope — the vectorscope and waveform need very different
intensity and follow different marker-smoothing rhythms.

## Platform notes: macOS

- Capture uses ScreenCaptureKit with a bare display filter. The exclusion
  variants of `SCContentFilter` snapshot their lists at creation time —
  windows or applications appearing later are silently absent from the
  capture. Self-capture feedback is handled app-side instead: the change
  hash masks out the app's own window, so its own redraws never re-trigger
  analysis. This is load-bearing — without the mask, a full-screen region
  self-sustains analysis at tens of percent of a core on an otherwise idle
  screen.
- Streams die when the display configuration changes (lock screen, display
  sleep). The app treats capture as a service to be restarted, retries on
  failure, and never presents stale data as live: cursor sampling is
  suspended while capture is down.
- Screen-recording permission attaches to the bundle identity; a stable code
  signature keeps a single grant valid across rebuilds. Requesting pixels in
  sRGB makes the OS handle display color conversion; wide-gamut capture is a
  future, deliberate step.
- The window sits above document and panel windows (Quick Look previews
  float higher than ordinary floating windows) and joins all Spaces.

## Platform notes: Windows

- Capture uses DXGI Desktop Duplication. `DuplicateOutput1` requests a BGRA
  format so HDR (scRGB) displays still deliver ordinary 8-bit color, falling
  back to `DuplicateOutput` on older systems; the staging texture is recreated
  when the display mode changes.
- Self-capture is excluded at the window level: the scope window and its
  overlays set `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`, the Windows
  analog of the macOS capture exclusion, so the app's own pixels never feed
  back into analysis.
- The region border and picker are layered windows drawn with GDI+ and
  composited through `UpdateLayeredWindow` — click-through except over their
  grab handles (`WM_NCHITTEST`). Every size scales by the per-monitor DPI, and
  the app carries a PerMonitorV2 manifest.
- Context menus are native (`TrackPopupMenu`), matching macOS.

## License

SideScopes is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE).
