# Scopes

SideScopes provides five scopes and a color picker. Any combination can share
the window, one pane per instrument. Use the scope selector to choose and order
them, or use the keyboard shortcuts.

The plots analyze only the selected screen region. With no region selected,
their graticules remain visible but their traces are empty. The pointer readout
and markers can still sample a color anywhere on screen.

All levels are derived from the captured desktop pixels. SideScopes does not
have access to a source file, timeline, document profile, or signal before the
source application renders it. See
[Measurement boundary](#measurement-boundary) before using the scopes for
color-managed or signal-compliance decisions.

## Vectorscope

<aside class="shortcut-note">
<strong>Default shortcut:</strong> <kbd>V</kbd> shows the vectorscope by itself.
<kbd>Shift+V</kbd> adds it to the current layout, or removes it when another
instrument remains.
</aside>

The vectorscope converts each captured RGB sample to two chroma coordinates.
Angle around the center corresponds to hue; radial distance corresponds to
chroma magnitude in this projection. Equal-channel neutrals lie at the center.
Level and image position are discarded, so light and dark samples with the
same chroma can occupy the same position.

Trace brightness represents sample density. A strong extension toward a hue
usually identifies a population of pixels in that direction. An off-center
neutral reference indicates chroma bias; an off-center whole-image trace may
instead describe the subject or an intentional grade.

The labeled boxes mark reference positions for 75% Rec.709 color-bar values;
the smaller unlabeled boxes mark the full-value positions. They are not gamut
boundaries or saturation limits. The skin-tone indicator is a directional
reference for comparison, not a target that every complexion should meet.

![Vectorscope showing a compact trace near the center of the graticule.](https://media.sidescopes.org/shots/vectorscope-neutral.png)

**Zoom** magnifies the central area at 1x, 2x, or 4x. It changes the view, not
the underlying coordinates.

SideScopes uses one fixed, full-range Rec.709-style RGB-to-Cb/Cr matrix. This
is most directly interpretable for SDR, sRGB/Rec.709-like rendered output. It
does not make the vectorscope a color-managed gamut test.

## RGB waveform

<aside class="shortcut-note">
<strong>Default shortcut:</strong> <kbd>W</kbd> shows the RGB waveform by itself.
<kbd>Shift+W</kbd> adds it to the current layout, or removes it when another
instrument remains.
</aside>

The RGB waveform overlays red, green, and blue channel levels. Its horizontal
axis follows the selected region from left to right; its vertical axis is
captured channel code value, normalized from 0 at the bottom to 100 at the top.
This is a full-range display-code scale, not a legal-range video scale.

Each sampled pixel contributes at its actual channel level. A vertical slice
therefore shows the values present in the corresponding image column; the
scope does not fill every value between the lowest and highest samples.

Channel separation is meaningful in context. Over a verified neutral, it can
show a rendered color bias and the tonal range in which that bias occurs. Over
a colored subject, the same separation may simply describe the subject.

![RGB waveform showing the red, green, and blue channels overlaid.](https://media.sidescopes.org/shots/waveform-rgb.png)

A strong accumulation at 0 or 100 means captured output pixels reached that
endpoint. It may be consistent with clipping or crushing in the rendered
output, but does not identify the limiting stage or prove that a RAW file,
timeline, or source signal has no recoverable detail.

## Luma waveform

<aside class="shortcut-note">
<strong>Default shortcut:</strong> <kbd>L</kbd> shows the luma waveform by itself.
<kbd>Shift+L</kbd> adds it to the current layout, or removes it when another
instrument remains.
</aside>

The luma waveform retains the waveform's horizontal image position and plots a
single weighted level:

`Y′ ≈ 0.2126 R′ + 0.7152 G′ + 0.0722 B′`

The calculation uses the encoded captured channel values. Y′ is not physical
luminance, scene brightness, or a direct exposure measurement. It is useful
for locating and comparing level distributions in the rendered output.

![Luma waveform showing the tonal structure of a snow-covered forest.](https://media.sidescopes.org/shots/waveform-luma.png)

**Plain** draws a neutral trace. **Colored** tints the trace from the captured
RGB contributions while keeping the same luma coordinate.

## RGB parade

<aside class="shortcut-note">
<strong>Default shortcut:</strong> <kbd>R</kbd> shows the RGB parade by itself.
<kbd>Shift+R</kbd> adds it to the current layout, or removes it when another
instrument remains.
</aside>

The RGB parade places the three channel waveforms side by side: red, green,
then blue. Each panel maps the full width of the selected region. The image is
not divided into thirds.

![RGB parade showing the red, green, and blue channel waveforms side by side.](https://media.sidescopes.org/shots/parade-clipping.png)

The separate panels prevent overlapping traces from hiding one another. This
makes channel shape, level, and endpoint behavior easier to compare. As with
the overlay waveform, alignment indicates neutrality only for a subject that
is independently expected to be neutral.

## Histogram

<aside class="shortcut-note">
<strong>Default shortcut:</strong> <kbd>H</kbd> shows the histogram by itself.
<kbd>Shift+H</kbd> adds it to the current layout, or removes it when another
instrument remains.
</aside>

The histogram counts how many sampled red, green, and blue values fall into
each of 256 bins. Minimum channel values are at the left and maximum values at
the right. It discards image position.

**Per Channel** places the complete red, green, and blue distributions in
separate vertical bands. **Combined** overlays them at full height. The
displayed heights use a square-root response so a large population does not
hide smaller ones; height is therefore not linearly proportional to count.

![Histogram showing the red, green, and blue distributions in separate bands.](https://media.sidescopes.org/shots/histogram.png)

A peak identifies a large population at that code value. A gap identifies few
or no sampled values in an interval. Neither proves a particular processing
fault: gaps do not by themselves establish visible posterization, and endpoint
peaks do not identify where limiting occurred.

## Color picker

<aside class="shortcut-note">
<strong>Default shortcut:</strong> <kbd>C</kbd> shows the color picker by itself.
<kbd>Shift+C</kbd> adds it to the current layout, or removes it when another
instrument remains.
</aside>

The color picker places the live sample beside pinned references. It reports
captured RGB values and, when a reference is selected, an assumed-sRGB color
difference.

Click to pin the pixel under the pointer. Drag to pin the average of a
rectangle, which is generally more repeatable for textured or noisy material.
Hold Shift to keep the pin tool active for additional samples.

![Color picker showing a live RGB sample and its comparison fields.](https://media.sidescopes.org/shots/color-picker.png)

SideScopes converts the reference and live samples from assumed sRGB to CIELAB
under D65. It reports CIEDE2000 **ΔE00** and three signed diagnostics:

- **ΔL\***, the CIELAB lightness difference;
- **ΔC\***, the CIELAB chroma difference;
- **ΔH\***, a chroma-weighted hue-difference term.

The components do not correspond one-to-one with exposure, saturation, or
white-balance controls, and the ΔE00 total is not their simple sum. There is no
universal acceptable ΔE00 threshold; the relevant tolerance depends on the
colors, sample area, viewing conditions, and task.

## Markers and pins

The color under the pointer is marked on every visible compatible scope: its
chroma position on the vectorscope, channel or luma level on waveforms, and
bin on the histogram. Markers work with or without an analysis region.

Pinned colors appear on the vectorscope and in the color picker. Pins persist
between sessions and can be selected as comparison references.

## Trace controls

Right-click a scope pane to reach that instrument's options directly.
Right-click elsewhere to open the complete menu.

**Graticule strength** changes the opacity of grid lines, rings, target boxes,
and labels. The choices are Faint, Soft, Normal, and Bold.

**Zoom** changes vectorscope magnification between 1x, 2x, and 4x.

**Style** selects Plain or Colored for the luma waveform, and Per Channel or
Combined for the histogram.

**Quality** sets capture cadence and analysis detail. Standard, the default,
targets 15 captures per second. High targets 20, retains full detail while a
region is dragged, and uses finer analysis for vectorscope and histogram panes
where their size benefits from it. High uses more processing time.

The Settings window opens from the right-click menu, or with Command+comma on
macOS. The waveform and parade share one set of trace settings because they
render the same channel measurement in different arrangements.

**Intensity** controls how much each sample contributes before the rendered
trace saturates. Scroll over a vectorscope or waveform to change it;
double-click to restore the default. It changes visibility, not coordinates.

**Sampling stride** samples every Nth pixel, from 1 through 8. A stride of 1
includes every pixel. Larger strides reduce analysis cost and may make sparse
or highly regular content less exact.

**Smoothing** controls how long the pointer marker takes to settle after the
sample changes, from 0 to 500 milliseconds. It affects the marker, not the
scope trace.

**Trace gamma**, available for the vectorscope, changes the density response
used to display the trace. Lower values reveal sparse populations more
strongly; higher values preserve greater contrast against the densest areas.
The range is 0.40 to 1.40.

## Arranging panes

Scopes divide the window in the order shown in the scope selector; drag rows
there to reorder them. The order belongs to the active preset. Turning a scope
off and on does not move it within that order.

The Layout menu controls the division axis. **Automatic** uses the longer
window axis; **Vertical (stacked)** and **Horizontal (side by side)** force an
orientation. Drag a divider to change the adjacent panes' shares.

Nine preset slots store scope visibility, order, division, pane weights, and
per-scope style. A digit loads its slot. Use the preset list to load or rename
slots.

Changing the current arrangement does not overwrite its slot. The toolbar save
button lights while the arrangement differs from the loaded preset. Click it,
or press Command+S on macOS or Control+S on Windows, to update that slot.
Shift+1 through Shift+9 writes the current arrangement to another slot without
loading it.

The current working arrangement persists separately, including when it has not
been saved to a preset. Loading another slot intentionally replaces that
working arrangement without a confirmation prompt.

Trace intensity, sampling, smoothing, trace gamma, window position, and window
size are not part of a layout preset.

## Measurement boundary

SideScopes receives the composited screen-capture result. The desktop capture
is requested or labeled as sRGB where the platform API permits, and the scope
calculations treat the received values as full-range, sRGB/Rec.709-like output.

SideScopes does not inspect the source profile, timeline color space, display
ICC profile, printer profile, HDR metadata, or upstream signal range. It is
not a soft-proofing system, HDR reference monitor, gamut checker, or
source-signal compliance scope. Use the source application's own configured
scopes when those stages matter.
