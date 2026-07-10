# Changelog

All notable changes to this project are documented in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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
