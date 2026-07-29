#pragma once

#include "imgui.h"

namespace sidescopes {

/// @file
/// The shared look of the toolbar's drop-down rows.
///
/// Two buttons on that row open a list - the scope selector and the preset
/// picker - and they are the same gesture twice, so they are drawn the same
/// way: roomy rows, one quiet bar highlighting a whole row rather than its
/// parts lighting up separately, and the key that reaches the entry set
/// right-aligned and dimmed the way a menu shows an accelerator. Keeping the
/// treatment here is what stops the two drifting apart.

/// Where a rename field starts so that the NAME INSIDE IT lands on @p nameX.
///
/// A framed input insets its own text by the frame padding; a label has none.
/// Placing the field at the name's own x therefore puts its text one padding
/// to the right of where the name was - the box in the right place and the
/// glyphs not, which reads as the row jumping sideways the moment a rename
/// begins. The field is placed by its text instead.
[[nodiscard]] float renameFieldX(float nameX);

/// The width that field needs to end where a name column of @p nameWidth ends,
/// given it starts a frame padding earlier.
[[nodiscard]] float renameFieldWidth(float nameWidth);

/// The space a row keeps between the controls leading it and its name.
///
/// This, rather than the name's x, is what the two lists share. They lead with
/// different controls - the scope menu one checkbox, the preset list a pair of
/// icon buttons - so one shared x gives whichever list has the wider controls a
/// visibly tighter name, which is exactly what it did.
[[nodiscard]] float menuRowLeadingGap();

/// Where a row's name starts, given the total width @p leadingWidth of the
/// controls standing before it: past them, then @ref menuRowLeadingGap.
[[nodiscard]] float menuRowNameX(float leadingWidth);

/// The margin a right-bound key hint keeps from the row's right edge. Shared
/// for the same reason as @ref menuRowNameX: the two lists bind their keys to
/// one edge or they look subtly misaligned, which reads worse than being
/// obviously different.
[[nodiscard]] float menuRowKeyRightPad();

/// Pushes the row styling for the popup being drawn: rows with room to
/// breathe, a checkbox scaled to its label rather than dwarfing it, and every
/// per-item highlight suppressed - including the theme's loud selection blue -
/// because @ref drawMenuRowHover lights the whole row instead. Must be paired
/// with @ref popMenuRowStyle before the popup ends.
void pushMenuRowStyle();

/// Undoes @ref pushMenuRowStyle. The counts live with the pushes, so no caller
/// can pop the wrong number.
void popMenuRowStyle();

/// Whether the pointer is over the band of the row starting at @p rowTopY -
/// what a row's own controls ask before deciding how strongly to draw
/// themselves. It is the WHOLE row that answers, not the control, so a pen the
/// pointer has not reached yet brightens as soon as its row is entered.
[[nodiscard]] bool menuRowHovered(float rowTopY);

/// Draws the hover band behind a whole row whose content starts at @p rowTopY,
/// so hovering reads as one row rather than as a control and a label lighting
/// up apart. The band reaches a little above and below the row and runs nearly
/// the popup's full width - just shy of the border, past the content into the
/// window padding - for a native-menu-style highlight edge to edge. Skipped
/// mid-drag, where the insertion bar is the cue.
void drawMenuRowHover(float rowTopY);

/// Draws the band that marks the row starting at @p rowTopY as the chosen one,
/// for a list where exactly one is - which a whole tinted row says at a glance
/// and a marker glyph in a column of its own says only to someone who knows
/// what the glyph means.
///
/// Drawn AFTER @ref drawMenuRowHover and over the same band, and see-through,
/// so the chosen row under the pointer reads as both rather than as either:
/// chosen, hovered, and chosen-while-hovered are three different shades.
void drawMenuRowChosen(float rowTopY);

/// Draws @p key over the row just laid down, right-aligned @p rightPad in from
/// the row's edge and dimmed, the way a menu shows an accelerator. It is a
/// hint rather than a control, so it is drawn fainter than the row's text and
/// the caller is expected to leave real distance between it and any control
/// sharing that edge. An empty @p key draws nothing.
void drawMenuRowAccelerator(const char* key, float rightPad);

/// Lays an invisible catch over the rows already drawn, from @p listTop down to
/// where the cursor now stands and @p width across, so a drag released anywhere
/// over the list lands on one target rather than on whichever row happened to
/// be under it.
///
/// IT COVERS THE GAP UNDER THE LAST ROW, and that is not slack. Every drop
/// position but the last sits between two rows; the last one - after everything
/// - can only be aimed at below the final row, so the strip under it IS that
/// position and a catch stopping at the last row's edge cannot express it.
///
/// The cursor is put back by SUBMITTING something rather than by being moved.
/// The obvious way to write this - move up, submit, move back - ends on a bare
/// cursor move with nothing after it, and ImGui reads that as being asked to
/// grow the window around nothing. It says so in a red error window over the
/// popup, in release builds as much as local ones, since the tooltip is only
/// compiled out by IMGUI_DISABLE_DEBUG_TOOLS.
///
/// @return How far down the catch reaches. The drop does not need it; it is
///         what lets a test see that the last position is inside.
float layMenuRowDropCatch(const char* id, const ImVec2& listTop, float width);

/// An icon button that stands in a menu row: the toolbar's glyph and hover
/// box, but only as tall as the row it shares, so a row carrying one is no
/// taller than a row of plain checkboxes and the two menus keep one rhythm.
///
/// The glyph is ALWAYS drawn; @p emphasized only decides how strongly. False
/// draws it dim - present, but receding far enough that the row reads
/// name-first.
///
/// This is the resolution of a real tension, and it is worth knowing before
/// anyone reaches for a plain hide. A row action that appears on hover has to
/// hold its box open when it is not showing, or every name in the list steps
/// sideways as the pointer moves down it; hold the box open around nothing and
/// the list carries an empty gutter down its whole length instead. Dimming
/// escapes both at once: the space is occupied by something meaningful, so
/// there is no gutter, and nothing moves because nothing was ever hidden.
/// @return Whether it was pressed.
[[nodiscard]] bool menuRowIconButton(const char* id, ImTextureID texture, const char* tooltip, bool emphasized = true);

/// The width @ref menuRowIconButton takes, for a caller measuring its columns
/// before it draws them.
[[nodiscard]] float menuRowIconWidth();

}  // namespace sidescopes
