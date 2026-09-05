# Changelog

All notable changes to this project are documented in this file. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- Mac downloads contain both Apple silicon and Intel code and require macOS
  14 or later, matching the screen-capture APIs used by the application.
- Desktop archives and both Lab distributions include the notices for their
  bundled libraries and fonts.

### Fixed

- Saved settings retain fractional values and negative monitor coordinates,
  reject malformed numbers, and preserve the previous file if saving fails.
- Cancelling a region pick allows another pick immediately. Clearing a region
  also clears its attachment and face-tracking state, and delayed native
  callbacks cannot access a session after it closes.
- Scope analysis retries failed module operations without displaying stale or
  partially copied results. Changes to narrow regions and pixel formats now
  invalidate the content cache correctly.
- Windows capture reads the acquired texture's pixel format, and repeated face
  detection releases cached native factories before their apartment closes.
- Diagnostic recording can be changed safely while capture and analysis emit
  messages, including when a reporting subsystem is shutting down.
- The Lab keeps the latest image selection during overlapping loads, measures
  transparent images against the displayed black background, saves preset
  selections, and refreshes its engine when a newer build is available.

## [0.7.0] - 2026-08-17

### Added

- SideScopes Lab can adjust the supplied or locally loaded image while the
  scopes update. Temperature, tint, saturation, exposure, contrast,
  highlights, and shadows are applied to one shared image buffer, so the
  picture on screen and the pixels measured by the scopes stay identical.
  The controls are an observation aid rather than an emulation of a
  particular editor.
- A fresh desktop installation starts with Vectorscope and Waveform, a
  compact application window at the left of the primary display, and a
  moderate square global region in the open display area. The region is
  immediately movable and resizable and is recreated rather than persisted
  between launches.
- Live pointer markers on the scopes can be disabled with
  `show_cursor_markers=0` in `preferences.conf`, independently of the RGB
  readout and pinned markers.

### Changed

- The browser experience is named SideScopes Lab throughout the source,
  generated files, documentation, and interface. Its layout now adapts from
  a side-by-side desktop workspace to a vertical mobile workspace without
  reducing the image and scope panes to an unusable scale.
- Face-tracked regions have their own face binding icon and retain the
  window title as their label. Clicking the face icon stops tracking while
  preserving the current rectangle as a window-attached region; clicking the
  resulting pin makes it global.
- The application and scope documentation now begins with the default global
  region, separates macOS and Windows application shortcuts, uses the
  instrument names shown by the application, and explains default shortcuts
  beside each scope rather than appending unexplained letters to headings.

### Fixed

- The Lab region border now follows the same filled ring, hazard band,
  handles, and device-pixel stroke geometry as the native border, without
  doubled seams or asymmetric edges.
- The region close control remains attached to the border while the region is
  moved or resized on macOS, Windows, and in the Lab.
- Face detection on macOS now reads ten-bit screen captures correctly instead
  of interpreting their packed pixels as eight-bit input.
- The compact first-run placement survives the transition from a hidden GLFW
  window to the visible macOS window.
- The status-bar color swatch is optically centered with the RGB readout and
  keeps balanced spacing at one-, two-, and three-digit values.

## [0.6.0] - 2026-08-14

### Added

- SideScopes runs in a browser. The same scope engines and the same panes
  the desktop application draws, compiled to WebAssembly, over a
  photograph that stands in for the screen. Scopes, region, color picker,
  presets, keyboard shortcuts and the right-click menu all work as they
  do on a desktop; window attach and face detection do not, because no
  page can read another program's window, so those controls are absent
  rather than present and broken.
- A guided tour of the interface, one control at a time, with a way on
  and a way out. It opens on a first visit and remembers having been seen
  through or waved away, and a button reopens it. The stops name the
  keyboard bindings in force rather than the shipped defaults.
- The browser build also emits one self-contained file that opens from
  disk with no server, for trying the application without installing it.

### Changed

- The analysis worker can run its passes on the caller's thread instead
  of its own, for a host with no threads to give. The passes are
  identical either way; every desktop build still gives it a thread.
- The region border's grab zones come from the shared geometry both
  desktop borders already use. A small region keeps a band that moves it
  rather than only zones that resize it.

### Fixed

- A shortcut action that no host handles is now a compile error rather
  than a silence.

## [0.5.0] - 2026-07-30

### Added

- The luma waveform is a scope of its own, on L, beside the RGB waveform
  on W. One plots encoded Y' and the other plots the three captured channel
  levels, and both can stand on screen at once instead of one replacing the other. Plain or
  Colored is its own style choice; the waveform no longer carries one.
- A scope selector in place of the letter chips: a toolbar button whose
  popup lists every registered scope with a checkbox, and a row can be
  dragged to set the order the panes follow. The letters go on working
  as shortcuts.
- A Quality choice in the right-click menu. Standard is the default.
  High reads the screen twenty times a second rather than fifteen, keeps
  full detail under the hand while a region is dragged, and computes the
  vectorscope image and the histogram plot at a finer step on a smaller
  pane. It buys nothing on the axes where measurement showed no
  difference. The preferences key is `quality`.
- A UI Scaling submenu, from 50% to 200% around Default. It multiplies
  the system scale rather than replacing it, so the operating system's
  own per-monitor scaling still leads and a window keeps the preference
  as it crosses displays. Until you choose a size, Default is the one
  the display's own density suggests rather than a flat 100%: a dense
  laptop panel the system leaves unscaled opens a step larger, a display
  the system already scales is left alone, and a size you have chosen is
  never overridden. The preferences key is `ui_scale_factor`.
- macOS reads the screen at ten bits a channel where the compositor
  offers them. The vectorscope is where it shows: a smooth gradient's
  chroma resolves into distinct positions instead of collapsing onto the
  eight-bit lattice, which is where banding was visible in the first
  place. The waveform and the histogram plot into 256 levels and are
  unchanged. This carries no HDR range - highlights clip where they
  clipped before - and a system that declines the deeper format degrades
  to whatever it does send. Windows is unchanged.
- A Trace gamma slider for the vectorscope, in Settings beside its
  intensity and sampling. It sets how hard the middle of the trace is
  lifted toward the densest chroma in the frame: lower brings the
  sparse body of the cloud up, higher leaves it nearer its own density.
  The range is 0.40 to 1.40 and it ships at 0.65, which draws exactly
  what every earlier build drew, so an untouched install sees no change.
- A diagnostic recording opens by stating what is already true - the
  capture format, its crop and its cadence, the scopes on offer and the
  letters they hold - rather than recording only what changes after it
  starts. Two channels join the others: `modules`, where a scope module
  that failed to load or was built for another ABI reaches the log
  instead of a console nobody can be asked to look at, and `interface`,
  carrying the errors the interface toolkit reports about its own use. A
  release build reports those into the log alone; the window it used to
  raise over the application stays in development builds.
- Documentation for the application in docs/: what each scope measures
  and how to read it, the region tools and the border on the desktop,
  the complete keyboard and mouse reference, and troubleshooting for
  permissions, interrupted capture and diagnostic logs.
- `SIDESCOPES_PREFS_FILE` places the preferences file, so a throwaway
  instance can take its own scope stack and window placement without
  disturbing the real one.
- Module ABI 0.5. A frame states how its pixels are packed, and the
  field carrying them is renamed, so a module built against this header
  cannot go on reading a ten-bit frame as bytes. A descriptor may
  declare a continuous parameter bounded by its own minimum and
  maximum, which the host draws as a slider. Two host extensions:
  accumulation scratch a scope borrows for the length of a pass instead
  of holding for its life, and the sample thinning the quality levels
  drive.
- A new application icon: the SideScopes mark, six vectorscope target
  hues ringing the square aperture of a selected region. macOS carries
  the plated cut that sits evenly among other Dock icons and Windows the
  free-form mark, and both go to a wider-channel drawing at the smallest
  sizes so the shape still reads. The mark and the name are not covered
  by the project's license; see `assets/brand`.

### Changed

- The scopes read only a region you have selected. With none they stay
  empty, keeping their graticule, and the marker and the color readout
  go on following the pointer anywhere on screen, so a color can still
  be measured against the graticule with nothing selected.
- Escape, the last region tool, and the right-click menu clear the
  region instead of returning it to the whole screen. The menu entry is
  Clear Region, and the preferences key is `shortcut_clear_region`.
- The application spends processor time only on what changed. With no
  region selected it captures, analyzes and draws nothing at all, and it
  stands down the same way while the window is hidden or minimized,
  while the display sleeps, and while the screen is locked. Capture is
  cropped to the region instead of taking the whole display, the screen
  is read fifteen times a second rather than thirty, the interface
  redraws at most twenty times a second, and presentation stops outright
  while nothing can move. An idle session costs a small fraction of a
  processor core where it used to cost more than two, and holds
  substantially less memory. Most of that is work that no longer happens
  rather than work made faster, though the analysis pass itself is
  cheaper too.
- The panes follow one order you keep, rather than the order the scopes
  were switched on. The selector lists every scope, shown or not, so
  checking and unchecking several never moves a row under the pointer,
  and a scope switched back on returns to the place it was left. The
  right-click Scopes list goes in the same order, and the order belongs
  to the preset, so two slots can lay the same scopes out differently.
  The preferences key is `scope_order`.
- Preset slots go by name rather than by the letters of the scopes they
  hold. A slot is called Preset 1 until it is renamed, which the pen at
  the head of its row in the picker does. The name persists, survives a
  save over the slot, and can be given before the slot holds a layout.
- Saving a layout is one action aimed at different slots. The toolbar
  carries a save button beside the preset picker: it lights when what is
  on screen differs from the slot it came from and is dark when there is
  nothing to save, which is the only place the interface says so - the
  asterisk the preset chip carried is gone. Cmd+S, or Ctrl+S on Windows,
  saves into the slot you are on and Shift with a digit into one you are
  not, and neither moves you.
- What is on screen is kept whether or not it has been saved. Quit with
  the save button lit and the next session opens on the same slot, the
  same arrangement and the same lit button. The slots and the layout
  being worked on persist separately, so a mistyped scope letter is an
  annoyance rather than the loss of a preset - nothing reaches a slot
  without being asked. A first run opens on Preset 1 like any other
  session, and a slot nothing has been saved into opens on the
  vectorscope alone.
- The preset control is a button carrying a panel glyph and the slot it
  is on, instead of a bare digit that looked like a label rather than
  something to click. It stands after the scope selector rather than
  before it: a preset is a set of scopes, and scopes are switched far
  more often than a preset is loaded. In its list the loaded slot is the
  row that is tinted, and the row whose pen stands at full strength
  while the others recede.
- The graticule has a strength instead of an on/off: the right-click
  menu offers Faint, Soft, Normal and Bold, dimming lines, rings, target
  boxes and labels together so it can be quietened over a busy trace.
  Normal is the strength the scopes are graded at, the faintest still
  reads, and the preferences key is `graticule_strength`.
- A region being dragged is followed by its border as it moves, rather
  than trailing the hand by a frame or two. Moving an attached window
  holds the scopes on their last reading until it lands, instead of
  analyzing a region in transit. Moving the region itself goes on
  reading live, at a coarser image while it is moving - except the
  waveform, which keeps every column so a highlight or a skin tone stays
  where it is while you scan for it.
- The word for choosing a window or a face is select rather than attach,
  in the toolbar, the menus and the picker, and the actions that end an
  attachment say stop following rather than detach. Attaching is the
  mechanism; what you are doing is choosing what the scopes read.
- A transient status message stands for five seconds rather than two. It
  sits in the bottom bar over the live readout, so it has to be read in
  one glance, and two seconds was not enough for a sentence. The two
  notices an attached region leaves - a window closing out from under
  it, a lost face - now appear there with every other message instead of
  above the panes.

### Removed

- The full-screen region and the Watch Full Screen action. A session
  starts with nothing selected rather than with the whole display.
- The vectorscope's Trace Response choice. Linear emulated a phosphor
  tube, whose faint trace was legible because the glass held it between
  sweeps; a single frame on a screen renders it close to black. Boosted
  is what the scope has always been read on, and the waveform has never
  offered anything else. Its curve is now the Trace gamma slider above.
- The vectorscope's Matrix choice. It now uses one explicit, full-range
  Rec.709-style projection for the SDR, sRGB/Rec.709-like display output the
  application expects. Screen capture supplies no source-signal metadata from
  which to select a different matrix reliably.
- The waveform's Style choice. Its two luma styles are the luma waveform
  above, which stands beside the waveform rather than in its place.
- The Save Current To menu, and saving from a preset row by Shift+click.
  A row loads; the save button and the two chords save.
- Scope letters from the preferences file. A stack and a preset's order
  name each scope by its id - `[org.sidescopes.waveform]` - rather than
  by the letter it answers to. A letter is a property of a scope and not
  its identity: the registry hands one out only if it is still free, so
  a collision or a change in the order modules register in would
  silently re-point every token already written, and users can rebind
  keys besides. The file is longer and says what it means.

### Fixed

- Screen capture is retried whenever it is wanted and gone, instead of
  standing on "Reconnecting automatically..." while nothing reconnected.
  A start that failed with nothing running yet was never retried at all,
  so a launch that reached the displays before they were ready left the
  scopes empty for good. The first attempt is as prompt as it ever was,
  at two seconds, and later ones widen to a ceiling of five while a
  display stays away.
- A column of one flat tone no longer dims the rest of the waveform or
  the parade. Sliding a region a few pixels off the picture onto the
  editor's chrome raised the ceiling the trace is normalized against
  several fold, dimming everything else by a quarter.
- The color readout reads at full strength away from the captured
  stream. A one-shot screen sample comes back letterboxed, and averaging
  its padding in reported every color at the fraction of the buffer the
  content covered - white read as 56% on a 16:9 display. This affected
  displays other than the one being captured.
- Clicking a preset slot nothing has been saved into loads the
  arrangement the application opens on - the vectorscope alone - rather
  than putting "preset N is empty" on the status bar and doing nothing.
  On a first run that was eight slots of nine offering an action and
  then refusing it.
- macOS: the keyboard comes back when an overlay hides. Finishing a pick
  - a confirming click or drag as much as Escape - or clicking the
  region border's band left the application with no key window, and
  every shortcut dead until a window was clicked again.
- Windows: the region border stays above windows that open after it.
  Topmost there is a position in the stacking order rather than a
  property that holds, so a window entering the band afterwards took the
  place above the border and kept it until the region itself changed.
- Windows: the desktop is no longer offered as a window to scope. It
  passes every rule the listing applies, and because it belongs to the
  same process as every File Explorer window and is larger than all of
  them, offering it also dropped those windows from the picker.
- Windows: a cloaked window - a suspended store app, or one on another
  virtual desktop - no longer answers as the window in focus, which left
  an attached region following nothing on screen.

## [0.4.0] - 2026-07-22

### Added

- Flexible scope layout: orientation (Automatic, Vertical, Horizontal),
  weighted panes with draggable dividers (double-click equalizes), and
  layout presets on the digit keys - a digit loads, Shift+digit saves.
  Presets recall each scope's style choices along with the geometry.
  Automatic picks the split whose panes best match each scope's natural
  shape instead of following the window's longer axis.
- A preset chip leads the toolbar, starred when the layout drifts from
  the saved slot; its popup loads on click and saves on Shift+click.
- A status bar under the panes carries the live color: the pin tool in
  its left corner, the color under the cursor in its right, and that
  color's channels named and read between them, so none of it paints
  over a trace. A transient message clears the row and takes it whole,
  and the channels are the first reading dropped when a narrow window
  leaves no room. The region toolbox keeps a constant width -
  unavailable tools dim instead of vanishing - and right-aligns,
  wrapping to its own row on narrow windows.
- Module ABI 0.2: descriptors may declare a preferred pane aspect for
  the automatic layout and flag their images as pin targets; the pin
  tool now follows the declarations instead of hard-coded scope ids.
- Diagnostics gained a `perf` channel: frame body and present/vsync
  wait, analysis-pass duration, and capture inter-arrival cadence. The
  Record Diagnostic Log toggle captures it with the others, and
  `SIDESCOPES_DIAG=perf` selects it alone. Off costs one branch.
- Pinned colors survive a restart, along with the one chosen as the
  comparison reference; the preferences file lists them as hex.
- The color picker states its distance from the pinned color as a
  delta E figure, with the signed lightness, colorfulness and hue
  differences beside it. Every column carries a tooltip saying what it
  means in plain words rather than in color-science terms.

### Changed

- The pin-a-color tool sits in the status bar beside the color it
  samples, rather than in the region toolbox; the toolbox now holds
  only the tools that choose what the scopes read.
- The tools that draw a region carry pen glyphs: a framed pen for
  attaching a region to a window, a plain pencil for drawing a global
  one.
- The face picker is always available where the platform detects faces;
  the toolbar no longer dims it or reports presence in a tooltip. The
  picker overlay itself now says "No faces found on this screen" once a
  display's scan finds none, and stays silent until the scan completes.
- The face action reads the same everywhere: the toolbar, the picker
  banner, and the menu all say attach to a face.
- The preferences file names its shortcut keys after the actions the
  menus show: `shortcut_attach_window`, `shortcut_draw_region`,
  `shortcut_attach_face`, and `shortcut_full_screen`. A binding
  customized under one of the old names falls back to its default.
- Region is the one word for the rectangle the scopes read: the menu
  says Draw Region..., and the toolbar tooltip and picker banner ask
  you to draw a region rather than select an area.
- The diagnostics channel for face regions is named after the face
  locking it records: `SIDESCOPES_DIAG=facelock` selects the probe
  verdicts, and `facepin` selects nothing.

### Fixed

- Toolbar and status bar icons keep their place to the pixel as the
  window moves across the screen, instead of shifting against the text
  beside them.
- The status bar stays at the foot of the window on the screen-capture
  help pages, instead of sitting directly under their text.
- Switching focus away from a window carrying an attached region takes
  its border down on the focus event itself, rather than at the next
  scheduled tick, so the border no longer lingers over the window that
  replaced it. On macOS this covers application switches; a switch
  between two windows of one application keeps the previous latency.
- A face-locked window regaining focus no longer flashes the stale
  region border for the first probe's latency; the border waits for
  this activation's verdict, and the probe fires immediately instead
  of waiting out its cadence.
- Face picking suggests faces on every display, not only the one the scopes
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

- Border colors settled: neutral gray chrome beside the photo, with one
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
