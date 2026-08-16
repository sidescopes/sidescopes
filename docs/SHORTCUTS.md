# Keyboard and mouse

Unmodified letter keys control SideScopes while its main window is active.
Command, Control, or Option suppresses those single-letter shortcuts so system
and window commands continue to work. Shift modifies several SideScopes
actions.

## Scopes

| Key | Action |
| --- | --- |
| V | Show the vectorscope |
| W | Show the RGB waveform |
| L | Show the luma waveform |
| R | Show the RGB parade |
| H | Show the histogram |
| C | Show the color picker |
| Z | Cycle vectorscope zoom: 1x, 2x, 4x |

A scope key shows that instrument by itself. Hold Shift to add it to the
current layout, or to remove it if already visible. At least one scope remains
open. Pane order follows the scope selector, regardless of the order in which
the shortcuts were pressed.

## Region

| Key | Action |
| --- | --- |
| A | Select or draw a window-attached region |
| D | Draw an unattached region |
| F | Select a detected face |
| Escape | Clear the region |

While a region picker is open, A, D, and F switch its mode without closing it.
Escape closes the Settings window first when that window is open; otherwise it
clears every attached and unattached region. It does not restore a previous
selection.

On the desktop border, drag inside to move the region, drag a corner to resize
it, or drag an edge midpoint to move one edge.

## Color pins

| Key | Action |
| --- | --- |
| P | Open the color-pin tool |

Click to pin the pixel under the pointer. Drag to pin the average of a
rectangle. Hold Shift while clicking or dragging to keep the tool open for
additional pins. P has no effect when no visible scope accepts color pins.

## Layout presets

| Key | Action |
| --- | --- |
| 1–9 | Load that preset slot |
| Command+S / Control+S | Save the current layout to the active slot |
| Shift+1–9 | Save the current layout to that slot without loading it |

Saving to another slot does not change the active slot. The toolbar save
button remains lit when the on-screen arrangement still differs from the
active preset.

## Application window

| Key | Action |
| --- | --- |
| Command+W or the red window button (macOS) | Hide SideScopes |
| Command+comma (macOS) | Open Settings |
| Command+Q (macOS) | Quit |
| Control+W (Windows) | Minimize |
| Control+Q (Windows) | Quit |
| Alt+F4 or the close button (Windows) | Quit |

On macOS, the Dock icon or Command+Tab restores the application and its region
border after Command+W hides it.

## Mouse

- Scroll over a vectorscope or waveform to change trace intensity;
  double-click to restore its default.
- Right-click a scope pane for that instrument's options. Right-click elsewhere
  for the complete menu.
- Drag a divider to change adjacent pane sizes.
- Drag rows in the scope selector to reorder panes.
- While the color-pin tool is open, hold Shift during a click or drag to keep
  it active.

## Rebinding

The letter bindings are stored in the plain-text preferences file. Each value
is one letter A–Z, or `Escape`; invalid values are ignored and the default is
used. The keys are:

- `shortcut_attach_window`
- `shortcut_draw_region`
- `shortcut_attach_face`
- `shortcut_pin_color`
- `shortcut_vectorscope_zoom`
- `shortcut_clear_region`
- `shortcut_<scope id>` for each scope

The preferences file is located at:

- macOS: `~/Library/Application Support/SideScopes/preferences.txt`
- Windows: `%APPDATA%\SideScopes\preferences.txt`
