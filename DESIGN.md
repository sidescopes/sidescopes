# Design

This document grows alongside the implementation. It records the goals, the
architecture, and the reasoning behind decisions that are not obvious from
the code.

## Goals, in priority order

1. **Useful beside a real editor.** A compact, always-on-top window showing
   one scope at a time (vectorscope while balancing color, waveform while
   setting exposure), scoped to a user-selected screen region. Small-laptop
   screens are the design target, not the exception.
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

## Scope engines

Both engines accumulate into integer bins and map bins to display images with
a logarithmic curve normalized to the densest bin: sparse traces stay visible
while dominant content clearly dominates, and the mapping adapts per frame.
Trace intensity is exposed to users as a 0–100% control that is exponential
in the underlying gain, so every step feels the same size. Bin counts are
normalized by the number of samples, which keeps intensity settings stable
when the sampling stride changes.

The vectorscope plots BT.601 or BT.709 chroma with the classic graticule:
primary/secondary targets at 75% and 100%, and a skin-tone line. The waveform
plots luma and/or per-channel levels; RGB overlay is the default because
separated colored traces make color casts readable at a glance.

Every scope implements a single projection function that maps a color to
scope coordinates. Graticule targets, the cursor marker, and any future
indicators go through it, which is what guarantees overlays and data agree.

## The cursor marker

The color under the pointer is sampled from the captured frame (a small
neighborhood average), smoothed with a per-scope exponential moving average,
and drawn as a marker on every visible scope. The pipeline stays in floating
point end to end: quantizing intermediate values makes the marker visibly
dither between adjacent bins while it settles. A small snap window ends the
asymptotic tail of the average decisively.

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
- Screen-recording permission attaches to the bundle identity; the app ships
  as a signed bundle. Requesting pixels in sRGB makes the OS handle display
  color conversion; wide-gamut capture is a future, deliberate step.
- The window sits above document and panel windows (Quick Look previews
  float higher than ordinary floating windows) and joins all Spaces.

## Interaction model

Three layers, no native settings window to maintain per platform:

1. Direct manipulation for continuous values — scroll over a scope to adjust
   intensity, double-click to reset, drag on screen to select the region.
   Gestures show their value transiently and always have an obvious revert.
2. A native context menu (right-click) for modes and toggles.
3. A slim ImGui panel for diagnostics and the few remaining sliders.

Defaults matter more than options: shipped values come from calibration
against real editing sessions, per scope — the vectorscope and waveform need
very different intensity and follow different marker-smoothing rhythms.

## Licensing and funding

GPL-3.0-or-later. The README carries a non-regression pledge: free features
never become paid. To keep the option of funding development later (store
builds, optional paid additions), external contributions are accepted under
a CLA; the pledge is the counterweight that keeps that arrangement honest.
Potential extras are structured as separate modules from the start, which is
good architecture regardless of whether they ever exist.
