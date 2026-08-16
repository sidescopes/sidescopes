# Choosing a region

SideScopes analyzes one rectangular region at a time. At launch, a moderate
square global region appears on the display containing the application. With no
saved window placement, the compact application window opens at the left of the
primary display and the region is centered there. The square provides a neutral
starting area for landscape and portrait content. This is an ordinary region:
drag or resize its border, replace it with another selection, attach it to a
window, or clear it.

Opening a region tool dims the desktop and previews the candidate region on the
scopes before you confirm it.

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

An attached region follows the window when it moves. When the window becomes
smaller, the region is temporarily constrained to remain inside it; if space
returns, the saved geometry is restored. The region does not scale with the
window, which avoids changing the measured subject when an application panel
or window edge moves.

You can attach regions to several windows. SideScopes measures the region for
the focused attached window. If focus moves elsewhere, it uses the unattached
region when one exists. A closed attached window is removed.

## Select a face (F)

Where face detection is available, SideScopes presents detected faces as
suggestions. Selecting one creates a region that follows the face within its
window. If no face is detected, the picker reports that result instead of
creating an estimated region.

Face selection is a geometric convenience. It does not classify complexion,
evaluate skin color, or change how the scopes analyze the enclosed pixels.

## Editing the border

A confirmed region has a desktop border labeled with its attached window when
applicable. The border is interactive:

- drag the striped band to move the region;
- drag a corner handle to resize both axes;
- drag the midpoint of an edge to move that edge;
- use the close control to clear the region;
- use the attachment control to attach or detach the region in place.

The scopes update while the border is edited. The border hides while a picker
is open, while SideScopes is hidden or minimized, while an attached window is
being moved, and while face tracking is reacquiring its subject.

## No region

Escape, the clear-region toolbar action, and the right-click menu can remove
the selection. The scope traces then remain empty while their graticules stay
visible. Pointer sampling and compatible markers continue to work anywhere on
screen.

With no region selected, SideScopes suspends continuous region capture and
analysis. Clearing a region does not restore an earlier selection or default to
the entire screen. The next launch creates a new starter region.

## Multiple displays

The starter region appears on the display containing the SideScopes window. A
picker opens on the display under the pointer, and confirming a region there
moves capture to that display. A global region belongs to one display and is
cleared if that display's configuration changes. An attached region follows its
window across displays.

If the captured display disconnects or becomes unavailable, SideScopes pauses
the scopes and reports the interruption. Capture resumes when the display
returns.
