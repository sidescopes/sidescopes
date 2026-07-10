# Changelog

All notable changes to this project are documented in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Smart region selection: the picker outlines detected photo canvases and
  application windows; hover highlights one, a click confirms it, dragging
  still selects manually.
- Photo canvas detection: heuristic candidates for the image area inside
  editor chrome, powering region suggestions.

- RGB parade waveform style: the three channels side by side.
- Histogram view with per-channel bars and cursor value markers.
- Keyboard shortcuts: V/W/B/H switch views, A selects the area, F resets
  it, G toggles the graticule.

- The SideScopes macOS application: compact always-on-top window with
  vectorscope, waveform, and combined views, scroll-to-adjust intensity,
  native context menu, drag region selection, cursor color markers, and
  automatic capture recovery.
- macOS platform layer: ScreenCaptureKit capture, native context menus,
  drag region selection with a persistent click-through border, and desktop
  services.
- Preferences persisted between sessions: calibration, region, view,
  toggles, and window placement.
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
