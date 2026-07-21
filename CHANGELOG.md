# Changelog

All notable changes to this project are documented in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Flexible scope layout: orientation (Automatic, Vertical, Horizontal),
  weighted panes with draggable dividers (double-click equalizes), and
  layout presets on the digit keys - a digit loads, Shift+digit saves.
  Presets recall each scope's style choices along with the geometry.
  Automatic picks the split whose panes best match each scope's natural
  shape instead of following the window's longer axis.
- A preset chip leads the toolbar, starred when the layout drifts from
  the saved slot; its popup loads on click and saves on Shift+click.
- A status bar under the panes holds transient messages and the live
  color readout, so neither paints over a trace. The region toolbox
  keeps a constant width - unavailable tools dim instead of vanishing -
  and right-aligns, wrapping to its own row on narrow windows.
- Module ABI 0.2: descriptors may declare a preferred pane aspect for
  the automatic layout and flag their images as pin targets; the pin
  tool now follows the declarations instead of hard-coded scope ids.
- Diagnostics gained a `perf` channel: frame body and present/vsync
  wait, analysis-pass duration, and capture inter-arrival cadence. The
  Record Diagnostic Log toggle captures it with the others, and
  `SIDESCOPES_DIAG=perf` selects it alone. Off costs one branch.

### Changed

- The face picker is always available where the platform detects faces;
  the toolbar no longer dims it or reports presence in a tooltip. The
  picker overlay itself now says "No faces found on this screen" once a
  display's scan finds none, and stays silent until the scan completes.

### Fixed

- Face picking offers faces on every display, not only the one the scopes
  currently capture. The streamed display's faces still appear the instant
  the picker opens; each other display is grabbed once and scanned in the
  background, its faces filling in when ready.

## [0.3.0] - 2026-07-20

### Added

- Regions attached to windows: the window mode (A) clicks a window - or
  draws a region inside one - and the region rides with that window from
  then on, gluing to it through moves and resizes and appearing only
  while the window is focused. Preview panels owned by helper processes
  (Quick Look among them) attach like any other window. One region type
  at a time: picking a window parks the global region until the
  attachment ends.
- Face regions: picking a face (F) pins the region to it and follows the
  face through pans, zooms, and crops; a face that stays gone dissolves
  the region rather than leave it outlining stale content.
- The region border grew a label band: the window title (or the display
  name for the global region), a pin button that attaches or releases
  the region without re-picking, and the close button. The border fades
  and settles into place when it appears, takes the keyboard on click,
  and Escape dismisses it.
- One icon set on every platform, rasterized from embedded SVG sources:
  the toolbar and the border wear the same glyphs on macOS and Windows.
- Built-in diagnostics: a Diagnostics submenu in the context menu
  records a timestamped log of routing and border decisions ("Record
  Diagnostic Log", "Show Diagnostic Log"), and SIDESCOPES_DIAG selects
  individual channels for development use. Logging costs nothing when
  off and at most ten writes a second while recording.
- Windows: "Show in Screen Captures" in the Diagnostics menu makes the
  SideScopes windows visible to screenshots for the session; they
  normally hide from captures so the scopes never analyze themselves.

### Changed

- Border colors settled: neutral grey chrome beside the photo, with one
  warm tone reserved for transient cues (window hover, draw spotlight,
  edit veil), and the drag outline wears the same dashes as the settled
  border.
- Fixed-width values across the picker sit on their labels' baseline
  instead of riding above it.
- The developer dumps (window suggestions, face-pin verdicts) merged
  into the diagnostics channels; SIDESCOPES_DEBUG_SUGGESTIONS and
  SIDESCOPES_FACEPIN_LOG are gone.

### Fixed

- Windows: the border label sized in device pixels instead of doubling
  under display scaling, alt-tab no longer reroutes the region
  mid-switch or flickers the acrylic backdrop, and moving the border
  between regions of different sizes no longer flashes the outgoing
  frame.
- Focus routing holds a tracked window's region through a click's empty
  focus handoff, and a focused tracked window can no longer have its
  region stolen by a stale window order.
- Small regions stay movable: resize zones never take more than a sixth
  of an edge.

## [0.2.0] - 2026-07-17

### Added

- A scope-module architecture: every scope, the built-ins included, sits
  behind a C plugin boundary and describes its own menu options, sliders,
  gestures, graticules, and markers. An optional dynamic configuration
  loads scope modules from a directory at startup on every platform.
- Color values on the picker swatches as code, percent, and hex, and
  perceptual comparison of pinned colors: each pin shows its CIELAB
  difference from the live color in a labeled comparison table.
- An About window leading with the version; development builds append
  the git commit they were built from, a dirty tree marked with an
  asterisk.

### Changed

- Analysis runs in parallel across cores: the vectorscope and waveform
  passes complete two to four times faster, most visibly on modest
  hardware, with results identical to the single-threaded ones.
- Window picking respects stacking: windows buried under the ones above
  them are no longer offered, and where windows overlap the pick goes
  to the one actually visible under the cursor.
- Pinning a color matches what the live readout shows: a click pins the
  cursor sample, a drag still pins the average of the dragged area.
- Preferences moved to scope-scoped keys. Old files load unchanged;
  after the first save an older build falls back to defaults for scope
  settings while keeping the scope stack.

### Fixed

- Windows: fixed-width values sized to match the interface font instead
  of towering over their labels.
- A corrupt scope-module file no longer hangs the dynamic configuration
  on load; the file is skipped and logged.
- Setting SIDESCOPES_NO_CAPTURE_EXCLUSION lets screenshot tools capture
  the application's own windows for documentation.

## [0.1.0] - 2026-07-13

### Added

- Screenshot-style region selection: toolbar icons (and keys) open the
  picker to click a window (A) or draw an area over the dimmed screen (D);
  Escape resets to the full screen. Drawing shows the current region with
  handles for moving and resizing; picking highlights the window under the
  cursor with the system accent.
- Detected faces as picker suggestions, padded so the surrounding skin
  joins the sample. Detection runs locally via the platform's built-in
  detector.
- Pinned reference colors on the vectorscope (P or the context menu), for
  matching skin tones across photos; the context menu clears them.
- A pin tool in the toolbar, shown while the vectorscope or the color
  picker is on: the screen stays undimmed, the crosshair cursor itself
  carries a live swatch previewing the sample, a click pins a
  cursor-sized patch, and a drag pins the average of the dragged area -
  photographs are textured, so pins come from areas, never single
  pixels. Each click decides its own fate: a plain click or drag pins
  and closes, holding Shift pins and keeps picking - a run of pins
  ends with a plain click, or Esc. Pinning never touches the capture
  region, and the pin tool and the region tools never switch into
  each other midway.
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
- P opens the one pin tool - the single-or-multiple choice moved from
  the opening shortcut to each click's own Shift, made at the moment
  the user actually knows the answer. The pin actions live in a Pins
  submenu riding the vectorscope's and the color picker's own menu
  sections. Pinning the region average and pinning the cursor color
  blind are gone - the tool covers both: a drag averages any area
  without giving up the monitored region, and a click pins what the
  cursor swatch already shows.

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
