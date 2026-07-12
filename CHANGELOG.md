# Changelog

All notable changes to this project are documented in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Screenshot-style region selection: toolbar icons (and keys) open the
  picker to click a detected rectangle (A), draw an area over the dimmed
  screen (D), or reset to the full screen (F). Drawing shows the current
  region with handles for moving and resizing; picking highlights the
  rectangle under the cursor with the system accent.
- Rectangle detection that finds photographs displayed by any application:
  boundary-based, so it works for black-and-white and smooth images, sees
  low-contrast borders through antialiasing, survives occlusion by the
  scope window itself, and offers both a window and the photo inside it.
- Detected faces as picker suggestions, padded so the surrounding skin
  joins the sample. Detection runs locally via the platform's built-in
  detector.
- Per-application region memory: the confirmed region is remembered for
  the application it belongs to and leads the picker's suggestions when
  that application is back on screen.
- Pinned reference colors on the vectorscope (P or the context menu), for
  matching skin tones across photos; the context menu clears them.
- Scope toggles that stack: V, W and H show one scope alone, Shift stacks
  and unstacks it, and enabled scopes split the window.
- RGB parade waveform style: the three channels side by side.
- Histogram view with per-channel bars and cursor value markers.
- An application icon, generated from source.
- A guidance pane in place of empty scopes when the screen cannot be
  captured: missing-permission instructions with a button straight to the
  System Settings pane, or the reconnect status when capture drops.

- The SideScopes macOS application: compact always-on-top window with
  vectorscope, waveform, and histogram, scroll-to-adjust intensity,
  native context menu, cursor color markers, and automatic capture
  recovery.
- macOS platform layer: ScreenCaptureKit capture, native context menus,
  the region picker with a persistent click-through border, and desktop
  services.
- Windows executable identity: the application icon and the version
  metadata the shell shows in Properties and Task Manager.
- Preferences persisted between sessions: calibration, region, visible
  scopes, toggles, and window placement.
- Graticule geometry built from the engines' own projections: vectorscope
  rings, color targets and skin-tone line, and the waveform scale.
- Analysis worker: scope engines on a dedicated thread with change
  detection, settings versioning, and double-buffered output.
- Trace intensity control: a perceptually even 0-100% mapping over the
  engines' gain range.
- Cursor marker smoothing: neighborhood averaging with exponential
  smoothing and a snap window.
- Region change hash that skips re-analysis of unchanged content and masks
  the application's own window out of change detection.

### Fixed

- The region border always stays beneath the scope window, and never
  leaks into the frame the picker analyzes when re-selecting a region.
- Windows: region overlays repaint incrementally, so drawing a selection
  and moving or resizing the region border track the cursor instead of
  trailing it on high-resolution displays.
- Windows: cursor-only desktop updates no longer cost a full-screen
  copy, and the paced capture stream always publishes the newest frame
  instead of dropping the last one before the screen goes quiet.
