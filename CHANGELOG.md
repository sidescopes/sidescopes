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
- A pin tool in the toolbar, shown while the vectorscope or the color
  picker is on: the screen stays undimmed, the crosshair cursor itself
  carries a live swatch previewing the sample, a click pins a
  cursor-sized patch, and a drag pins the average of the dragged area -
  photographs are textured, so pins come from areas, never single
  pixels. A click pins one color and closes (P opens this flavor);
  Shift+click or Shift+P keeps picking until Esc. Neither flavor ever
  touches the capture region, and the pin tool and the region tools
  never switch into each other midway.
- Vectorscope view magnification: Z cycles 1x/2x/4x, scaling the trace,
  graticule, and markers together, with a badge naming the factor.
- A linear trace response option beside the default boosted curve: density
  maps to brightness the way a phosphor scope glows, and in both modes the
  densest mass blooms toward white - a neutral core reads at a glance.
- Colored luminance waveform style: the trace plots luma while carrying
  the average color of the pixels behind each point.
- The context menu shows keyboard shortcuts, offers every region action
  (pick, draw, faces, full screen), and tailors its options to the scope
  under the cursor - pinning actions appear only where pins mark
  something. Shortcuts are configurable in the preferences file, and the
  cursor readout's percent-versus-0-255 choice lives there too instead
  of in the menu.
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

### Changed

- The color picker pane is redesigned around comparison: a split
  comparator holds the live color against a selected pin edge to edge,
  values follow the percent preference with hex always at hand, and
  pins grow into a reference deck with per-channel deltas against the
  live color. Three size tiers keep it predictable from a sliver to a
  full window.

- The vectorscope defaults to the BT.709 matrix; BT.601 remains available
  in the menu. Integer rounding of the 709 coefficients now preserves the
  neutral axis exactly.
- All scopes share one graticule palette, brighter than before; the
  parade separates its three panes with dark gutters.
- The histogram draws dim solid fills under bright solid outlines that
  ride each channel's curve, so shapes stay traceable through every
  overlap and the plot sits quietly next to the other scopes. It sizes
  itself to its pane like the other scopes, and the per-channel style
  is the default - it reads like the parade and keeps the family
  consistent. Its curve outline strokes at display resolution, so the
  line keeps one width however the pane is sized.
- The region border hides while the window is minimized and returns on
  restore: minimized scopes measure nothing, and the border's grab band
  should not sit interactive over the editor meanwhile.
- P opens the pin tool and Shift+P its repeating flavor; the pin
  actions live in a Pins submenu riding the vectorscope's and the
  color picker's own menu sections. Pinning the region average and
  pinning the cursor color blind are gone - the tool covers both: a
  drag averages any area without giving up the monitored region, and
  a click pins what the cursor swatch already shows.

### Fixed

- System shortcut chords no longer trigger scope switches: Cmd+W closes
  the window on macOS instead of opening the waveform, and any
  Command/Control/Option combination leaves the plain-letter shortcuts
  alone on both platforms. Ctrl+Q quits on Windows, where Alt+F4 was
  the only affordance; Ctrl+W minimizes there. On macOS, Cmd+W and the
  close button dismiss the window while the application keeps running -
  the Dock icon brings it back with every setting intact - and Cmd+Q
  owns the real quit. Minimizing hides the region border along with the
  scopes on both platforms.

- Activating a scope no longer flashes garbage or stale data for its
  first frames: textures start blanked instead of holding recycled GPU
  memory, and turning a scope on waits briefly for the analysis worker
  to recompute - woken immediately instead of waiting out its frame
  timeout - so the first drawn frame already shows the current content.

- The region border always stays beneath the scope window, and never
  leaks into the frame the picker analyzes when re-selecting a region.
- Clicking the region border's band no longer strands the keyboard:
  on macOS the band hands the keyboard to the scope window, and on
  Windows the click no longer deactivates whichever window held it -
  shortcuts keep working through band drags on both platforms.
- Windows: region overlays repaint incrementally, so drawing a selection
  and moving or resizing the region border track the cursor instead of
  trailing it on high-resolution displays.
- Windows: cursor-only desktop updates no longer cost a full-screen
  copy, and the paced capture stream always publishes the newest frame
  instead of dropping the last one before the screen goes quiet.
- Windows: the interface paces itself off the desktop compositor;
  animation no longer burns a full processor core on some graphics
  drivers.
- Windows: the window restores its saved position and size instead of
  growing by the monitor scale on every launch, and always starts
  fully on screen - a window that began beyond the desktop edge used
  to drag a white, never-drawn strip into view.
