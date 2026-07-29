# Choosing a region

The scopes measure one rectangle of the screen. Choosing it works the way a
screenshot tool works: a tool dims the screen, you click or drag, and the
selection confirms. Whatever you hover previews on the scopes before you
commit to it, so a region can be found by looking rather than by guessing.

Three tools pick a region, from the toolbar or from the keyboard, and each
switches to another without closing the picker. The picker suggests only
exact information — real window rectangles and detected faces — and leaves
everything else to a manual draw, on the principle that an unreliable guess
is worse than an honest selection.

## Attach to a window (A)

Click a window and the region becomes that window, pulled in from its edges
far enough that the border's own chrome and label clear the title bar and
its buttons. Trimming a toolbar or a side panel out of the region is left to
you: drag the border, or drag inside the window in the first place to take
only part of it. Either way the region is glued to that window rather than
to the screen.

Attached, the region travels with the window as it moves. When the window
is resized the region keeps its size and only gets pushed out of the way by
an arriving edge, just enough to stay inside; grow the window again and the
region springs back to where it was. This is deliberate: a region resized
in proportion with the window would change what is being measured every
time a panel is dragged.

Several windows can be attached at once. Only the focused one is measured,
so switching between two applications switches which region the scopes
read, and focusing anything else falls back to the unattached region if
there is one. A window that closes is dropped, and the status row says so.

## Draw a region (D)

Drag a rectangle anywhere on the screen. It belongs to the screen rather
than to any window, so it stays where it was drawn no matter what moves
over it.

## Attach to a face (F)

Where the platform can detect faces, the picker offers the ones it found;
clicking one scopes the skin around it, and the region then follows that
face as it moves within the window. If the scan finds nothing it says so
rather than offering a guess.

## The border

Once a region is confirmed it carries a border on the desktop, drawn like a
screenshot selection and labeled with the window it belongs to. It is a
control, not a decoration: drag it to move the region, drag a corner handle
to resize it, drag the handle at the middle of an edge to move that edge
alone. The scopes follow live as you drag.

The border also carries a control that dismisses the region and one that
converts it between attached and unattached in place, so a region drawn by
hand can be glued to the window under it without being drawn again.

It hides itself when it would be in the way or would lie: while a picker
is open, while the application is hidden or minimized, while an attached
window is being moved, and while a face lock is hunting for a face it has
lost.

## No region

Clearing the region — Escape, the last tool in the toolbar, or the
right-click menu — leaves the scopes empty. That is a state rather than a
failure: the graticules stay, the marker and the color readout go on
following the pointer anywhere on screen, and the application stops
capturing and analyzing entirely until a region is chosen again.

Escape clears the region outright. It does not fall back to a previous
selection or to the whole screen.

## Several displays

The picker opens on the display under the pointer, and confirming a region
there moves capture to that display. A region is a rectangle on one
display, so it is cleared when the display it lives on changes. An attached
window carries its region with it across displays.

If a display disconnects, capture pauses and the scopes say so; it resumes
by itself when the display comes back.
