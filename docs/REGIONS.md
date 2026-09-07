# Choosing a region

SideScopes analyzes one rectangular region at a time. At launch, a moderate
square global region appears on the display containing the application. With no
saved window placement, the compact application window opens at the left of the
primary display and the region is centered there. The square provides a neutral
starting area for landscape and portrait content. This is an ordinary region:
drag or resize its border, replace it with another selection, attach it to a
window, or select a face.

Opening a region tool dims the desktop and previews the candidate region on the
scopes before you confirm it. Cancelling restores the previous committed
selection, including when no face is found.

The toolbar and keyboard provide three selection modes. You can switch among
them while the picker is open. Window and face suggestions are offered only
when the platform reports or detects them; otherwise you can draw the region
directly.

## Global region (D)

Drag a rectangle anywhere on the current display. A global region remains at
that screen position when windows move beneath it. Use this mode for any fixed
area of the desktop, including part of an application window.

The launch region is a global region and behaves exactly like one drawn with D.
It is centered on the display containing the SideScopes window when that space
is clear. If a saved application-window position occupies the center, the
region moves into nearby open space instead.

## Select a window (A)

Click a suggested window to create a region attached to it. SideScopes insets
the initial rectangle so its own border and label do not cover the title bar.
To exclude application controls, draw inside the window instead or edit the
border after selection. Either method can produce a window-attached region.

An attached region follows the window when it moves. A shrinking window edge
pushes the region inward. If the window becomes smaller than the region, the
visible rectangle is clipped; its saved size returns when space allows. The
region does not scale with the window, which avoids changing the measured
subject when an application panel or window edge moves. Minimizing and
restoring a window preserves the crop it held before the animation.

You can attach regions to several windows. SideScopes measures the region for
the focused attached window. If that window is minimized, unavailable, or
no longer focused, analysis pauses until an attached window is available.
When the last-used parent is confirmed closed, its last valid rectangle stays
global on the same display. Closing a background parent does not replace a
region that is still active.

## Select a face (F)

Where face detection is available, SideScopes presents detected faces as
suggestions. Selecting one creates a region that follows the face within its
window. If no face is detected, the picker reports that result instead of
creating an estimated region. Cancelling keeps the prior selection.
If following later loses the face, its last accepted rectangle remains attached
to the window.

Tracking follows changes in position and size as new video frames arrive. A
brief missed detection keeps the last rectangle visible while tracking tries
to recover. Persistent loss, an ambiguous crossing, or a changed capture source
ends following and keeps an ordinary attached region. Select the face again
with F to resume following; another face entering the area does not restart it.
Fast cuts, occlusion, and large changes in pose can end tracking.

A face-tracked border uses the same face icon as the selection tool. Its label
remains the window title; tracking state is not added to the title text. Click
the face icon to stop following the face while keeping the current rectangle
attached to its window. The region does not return to the face's original
position. Click the resulting pin icon to make the region global.

Face selection follows nearby geometry rather than recognizing a person's
identity. It does not classify complexion,
evaluate skin color, or change how the scopes analyze the enclosed pixels.

## Editing the border

A confirmed region has a desktop border labeled with its attached window when
applicable. The border is interactive:

- drag the striped band to move the region;
- drag a corner handle to resize both axes;
- drag the midpoint of an edge to move that edge;
- use the binding control to change what the region follows.

The binding control shows the current state. The face icon identifies a
face-tracked region, the pin identifies a region fixed inside a window, and the
struck-through pin identifies a global region fixed to the display. Clicking
the face icon stops face tracking and leaves the current rectangle attached to
its window; clicking the resulting pin makes the region global. On a global
region, the control attaches to the frontmost window under the region's center.

The scopes update while the border is edited. The border hides while a picker
is open, while SideScopes is hidden or minimized, and while an attached window
is being moved. Face motion keeps the border visible.

## Keeping a region

Escape cancels an active picker and restores the committed selection. It does
not erase a selected region. Stop Following Window and Stop Following All
Windows keep the last-used rectangle as a global region on its display.

If no valid source is available, the scopes pause while their graticules remain
visible. The application keeps an internal empty fallback for unavailable
displays and capture failures; it does not invent a region on another display.

## Multiple displays

The starter region appears on the display containing the SideScopes window. A
picker opens on the display under the pointer, and confirming a region there
moves capture to that display. A global region belongs to one display. An
attached region follows its window across displays.

If the captured display disconnects or becomes unavailable, SideScopes pauses
the scopes and reports the interruption. Capture resumes when the display
returns.
