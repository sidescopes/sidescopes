# Keyboard and mouse

Plain letters are shortcuts, so a keystroke reaches the scopes without a
modifier. Holding Command, Control or Option silences them, leaving those
chords to the system and the window; Shift alone stays meaningful, and
stacks.

## Scopes

| Key | Action |
| --- | ------ |
| V | vectorscope |
| W | waveform |
| L | luma waveform |
| R | RGB parade |
| H | histogram |
| G | combined histogram |
| C | color picker |
| Z | step the vectorscope's zoom: 1x, 2x, 4x, round again |

A letter shows its scope alone. Holding Shift adds it alongside the ones
already up, or removes it when it is up already; the last one on screen
stays. Panes follow the order set in the scope selector, not the order the
scopes were switched on.

## The region

| Key | Action |
| --- | ------ |
| A | attach to a window |
| D | draw a region |
| F | attach to a face |
| Esc | clear the region |

Each opens a picker over the dimmed screen, and the picker's own keys
switch between the three without closing it or disturbing the region.

Escape clears the region, whether a picker is open or not: every attached
window is detached and the region dropped, leaving the scopes reading
nothing. It never falls back to a previous selection. When the Settings
window is open, Escape closes that first.

The region's border on the desktop is a control in its own right: drag it
to move the region, drag a corner handle to resize, drag an edge's midpoint
handle to move that edge alone.

## Colors

| Key | Action |
| --- | ------ |
| P | pin the color under the cursor |

The pin tool closes after one pin; Shift-clicking pins and keeps it open.
Dragging pins the average of the rectangle rather than a single pixel.

P is silent when no scope that takes pins is on screen.

## Layouts

| Key | Action |
| --- | ------ |
| 1 - 9 | load that preset slot |
| Shift+1 - 9 | save the live layout into that slot |

## The window

| Key | Action |
| --- | ------ |
| Command+W, red button (macOS) | hide the application |
| Command+comma (macOS) | open Settings |
| Command+Q (macOS) | quit |
| Control+W (Windows) | minimize |
| Control+Q (Windows) | quit |
| Alt+F4, the close button (Windows) | quit |

These follow the platform's own conventions rather than the application's
preferences. On macOS, Command+W hides through the system, so the Dock icon
or Command+Tab brings everything back, the region border included.

## Mouse

- Scroll over the vectorscope or a waveform to change how brightly its
    trace is drawn; double-click to return it to the default.
- Right-click anywhere for the menus. Right-clicking a scope's own pane
  offers that scope's options first, since the click says which scope you
  mean.
- Drag the divider between two panes to change their share of the window.
- In the scope selector, drag a row to set the order the panes follow.

## Rebinding

Every letter above is a default, not a fixture. The preferences file holds
one line per binding — `shortcut_attach_window`, `shortcut_draw_region`,
`shortcut_attach_face`, `shortcut_pin_color`, `shortcut_vectorscope_zoom`,
`shortcut_clear_region`, and `shortcut_<scope id>` for each scope. A value
is a single letter A-Z, or `Escape`. Anything else is ignored and the
default stands.

The file is `preferences.txt`, under `Application Support/SideScopes` on
macOS and `%APPDATA%\SideScopes` on Windows.
