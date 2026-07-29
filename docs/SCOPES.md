# The scopes

Six scopes and a color picker. Any combination of them can be on screen at
once, splitting the window into panes; the scope selector in the toolbar
chooses which, and the letters do the same from the keyboard.

Every scope measures the selected region and nothing else. With no region
selected they stay empty, keeping their graticule, while the pointer marker
and the color readout go on following the cursor anywhere on screen — so a
color can still be measured against the graticule with nothing selected.

## Vectorscope (V)

A density plot of color with brightness thrown away. Distance from the
center is saturation and the angle around it is hue, so a neutral image is
a small cloud in the middle and a color cast is that same cloud pushed off
center in the direction of the cast.

The graticule carries the classic targets — the primaries and secondaries
at their 75% and 100% positions — and a skin-tone line running from the
center toward orange. Human skin sits along that line whatever its color,
which is what makes it useful: the tone can be lighter or darker, but it
should not drift off the line.

Zoom magnifies the plot 1x, 2x or 4x around the center for when the
interesting part of the trace is the neutral core rather than the saturated
edges.

The measurement is BT.709, and there is no second matrix to choose from,
because on a computer display there is no reasonable second answer: sRGB's
color primaries are BT.709's. BT.601's are tied to television phosphors
from 1953 and would put the targets where nothing on the screen is.

## Waveform (W)

Levels across the width of the region. The horizontal axis is the region's
own horizontal axis, so a feature on the right of the image is on the right
of the trace; the vertical axis is level, black at the bottom and white at
the top. A column of the image holding both bright and dark pixels draws
the whole range between them.

The three channels are drawn overlaid in their own colors, which is what
makes a cast readable at a glance: a trace whose blue rides above its red
at the top is a cool highlight, and the gap between them is how cool.

## Luma waveform (L)

The same plot, of brightness alone, which is what to read for exposure. The
top of the trace is the brightest pixel in each column and the bottom the
darkest, so an uneven exposure shows as a slope and a flat subject as a
band.

The waveform reads balance and this one reads exposure. They are separate
scopes so that both can stand on screen at once rather than one replacing
the other.

The Colored style tints each column with that column's own average color,
which puts a hint of the balance back without separating the channels.
Plain draws it in white.

## RGB parade (R)

The same waveform measurement with the three channels laid side by side
instead of overlaid: red, then green, then blue across the pane. Overlap
hides things, and three separate traces let you compare their shapes
exactly — which is the usual way to see which channel clips first, or that
the shadows of one channel sit higher than the others.

## Histogram (H) and combined histogram (G)

How many pixels sit at each level. The per-channel histogram stacks red,
green and blue in bands, each shape complete with nothing occluding it. The
combined histogram overlays all three at full height, where the overlap
itself is the information — a gray subject shows the three curves lying on
top of one another.

They are separate scopes rather than one drawn two ways because the two
questions come up at different moments, and having both on screen answers
them at once.

Bar heights follow a square-root curve, the way photo editors draw a
histogram, so one tall peak does not flatten everything around it.

## Color picker (C)

Not a plot but a comparison. The live color under the pointer sits beside a
color you pinned earlier, with the difference between them given as ΔE
(CIEDE2000) and split three ways: how much lighter or darker the live color
is, how much more colorful or duller, and how far its hue has drifted. Hue
drift counts for less when the color is dull, which is what the eye does
too. Every value is one click to copy.

This is the mechanism for matching across images. Pin a color in one
photograph, open the next, and hover the tone that should match it.

## Markers and pins

The color under the pointer is drawn as a marker on every visible scope at
once — its position on the vectorscope, its level on the waveform, its bin
on the histogram — which is how a single tone can be read against all of
them together. The marker works with or without a region selected.

The pin tool keeps a color for later. Clicking pins the color under the
cursor; dragging pins the average of a rectangle, which is what a textured
photograph needs, since a single pixel of skin or sky is rarely
representative. Holding Shift keeps the tool open to pin several. Pinned
colors show on the vectorscope and in the color picker.

## The controls that shape a trace

### On the right-click menu

Right-clicking a scope's own pane offers that scope's options directly,
since the click says which scope you mean; right-clicking anywhere else
groups them under the scope they belong to.

**Graticule strength** dims or strengthens the overlay as a whole — lines,
rings, target boxes and labels together — so it can be quietened over a
busy trace. Faint, Soft, Normal and Bold; Normal is the strength the scopes
are graded at, and the faintest still reads.

**Zoom**, on the vectorscope, magnifies 1x, 2x or 4x around the center.

**Style**, on the luma waveform, chooses Plain or Colored.

**Quality** sets how often the screen is read and how finely the scope
images are computed. Standard is the default and reads the screen fifteen
times a second. High reads it twenty times a second, keeps full detail
while a region is being dragged, and computes the vectorscope and histogram
images at a finer step on a smaller pane. It costs more processor time.
Nothing else differs between the two, deliberately: measurement found no
gain on the other axes.

### By gesture, and in the Settings window

The Settings window opens from the right-click menu, or with Command+comma
on macOS. It lists three sets of sliders rather than six, because the
waveform and the parade are one measurement drawn twice and share their
tuning, while the luma waveform and the vectorscope each have their own.

**Intensity** is how brightly a trace is drawn — really how much a single
sample contributes before the plot saturates. Scroll over the vectorscope
or a waveform to change it, and double-click to return to the default; the
value shows briefly on the pane as you turn it. A low intensity shows only
where the image is dense, a high one brings out the sparse edges of the
cloud. The histograms have no such control: a bar is a count.

**Sampling stride** takes every Nth pixel instead of all of them, from 1 to
8. The scopes plot the same shape either way on a photograph, and a larger
stride costs less; 1 is exact.

**Smoothing** is how long the cursor marker takes to settle on a new color,
from 0 to 500 milliseconds. Too little and the marker jitters as the
pointer crosses a textured photograph; too much and it lags behind the
hand. Each set has its own, because the scopes are read at different
rhythms.

**Trace gamma**, on the vectorscope, sets how hard the middle of the trace
is lifted toward the densest chroma in the frame. Lower brings the sparse
body of the cloud up, higher leaves it nearer its own density. The range is
0.40 to 1.40.

## Arranging the panes

Scopes split the window in the order the scope selector lists them, and
that order can be dragged. The order belongs to the preset you are on, so
different slots can lay the same scopes out differently. A scope switched
off and back on returns to the place it was left.

The Layout menu chooses how the window is divided. Automatic splits the
longer axis, so panes stack in a tall window and sit side by side in a wide
one; Vertical (stacked) and Horizontal (side by side) force it either way.
Dragging the divider between two panes changes their share.

Nine preset slots hold layouts. A digit loads its slot; the picker in the
toolbar opens the same nine as a list, where clicking a row loads it and
the pen on each row renames it. A slot nothing has been saved into opens
on the vectorscope alone, which is where the application starts.

Nothing reaches a slot until you say so. Rearrange the scopes and the save
button beside the picker lights: the layout on screen now differs from the
slot it came from. Click it, or press Cmd+S (Ctrl+S on Windows), and the
slot takes it. That button is the only thing that says a layout is
unsaved, and while it is dark there is nothing to save.

What is on screen is kept for you regardless, saved or not. Quit while the
button is lit and you come back exactly where you were: the same slot, the
same arrangement, the same button still lit. The slots and the layout you
are working on are two different things that persist separately, which is
what lets a mistyped scope letter be an annoyance rather than the loss of
a preset — the slot was never written to.

Loading another slot replaces what is on screen and asks nothing. What was
unsaved was never in a slot to begin with, and you asked to go elsewhere.

A preset stores which scopes are shown, their order, the split, the pane
weights and each scope's style — but not intensity, sampling, smoothing or
trace gamma, which are tuning rather than layout, and not the window's
position or size.
