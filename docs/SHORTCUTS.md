# Keyboard and mouse

Unmodified letter keys control SideScopes while its main window is active.
Command, Control, or Option suppresses those single-letter shortcuts so system
and window commands continue to work. Shift modifies several SideScopes
actions.

## Scopes

| Key | Action |
| --- | --- |
| V | Show Vectorscope |
| W | Show Waveform |
| L | Show Luma Waveform |
| R | Show RGB Parade |
| H | Show Histogram |
| C | Show Color Picker |
| Z | Cycle Vectorscope zoom: 1x, 2x, 4x |

A scope key shows that instrument by itself. Hold Shift to add it to the
current layout, or to remove it if already visible. At least one scope remains
open. Pane order follows the scope selector, regardless of the order in which
the shortcuts were pressed.

## Region

| Key | Action |
| --- | --- |
| A | Select or draw a window-attached region |
| D | Draw or replace the global region |
| F | Select a detected face |
| Escape | Clear the region |

While a region picker is open, A, D, and F switch its mode without closing it.
Escape closes the Settings window first when that window is open; otherwise it
clears every attached and unattached region. It does not restore a previous
selection.

On the desktop border, drag the striped band to move the region, drag a corner
to resize it, or drag an edge midpoint to move one edge.

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

<div class="platform-shortcuts" data-platform-shortcuts>
<section data-platform="windows">
<h3>Windows</h3>
<table>
<thead><tr><th>Key</th><th>Action</th></tr></thead>
<tbody>
<tr><td>Control+W</td><td>Minimize</td></tr>
<tr><td>Control+Q</td><td>Quit</td></tr>
<tr><td>Alt+F4 or the close button</td><td>Quit</td></tr>
</tbody>
</table>
</section>
<section data-platform="macos">
<h3>macOS</h3>
<table>
<thead><tr><th>Key</th><th>Action</th></tr></thead>
<tbody>
<tr><td>Command+W or the red window button</td><td>Hide SideScopes</td></tr>
<tr><td>Command+comma</td><td>Open Settings</td></tr>
<tr><td>Command+Q</td><td>Quit</td></tr>
</tbody>
</table>

<p>The Dock icon or Command+Tab restores the application and its region border after Command+W hides it.</p>
</section>
</div>

The website initially shows the table for the detected platform. Both tables
remain available from the platform selector.

## Mouse

- Scroll over Vectorscope or Waveform to change trace intensity;
  double-click to restore its default.
- Right-click a scope pane for that instrument's options. Right-click elsewhere
  for the complete menu.
- Drag a divider to change adjacent pane sizes.
- Drag rows in the scope selector to reorder panes.
- While the color-pin tool is open, hold Shift during a click or drag to keep
  it active.

## Rebinding

The letter bindings are stored in the application preferences. Each value is
one letter A–Z, or `Escape`; invalid values are ignored and the default is used.
The keys are:

- `shortcut_attach_window`
- `shortcut_draw_region`
- `shortcut_attach_face`
- `shortcut_pin_color`
- `shortcut_vectorscope_zoom`
- `shortcut_clear_region`
- `shortcut_<scope id>` for each scope

The preferences file is located at:

- macOS: `~/Library/Application Support/SideScopes/preferences.conf`
- Windows: `%APPDATA%\SideScopes\preferences.conf`

This is a strict `key=value` configuration file, not a prose document. Quit
SideScopes before editing it and change only documented keys.
