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

/// Pushes the row styling for the popup being drawn: rows with room to
/// breathe, a checkbox scaled to its label rather than dwarfing it, and every
/// per-item highlight suppressed - including the theme's loud selection blue -
/// because @ref drawMenuRowHover lights the whole row instead. Must be paired
/// with @ref popMenuRowStyle before the popup ends.
void pushMenuRowStyle();

/// Undoes @ref pushMenuRowStyle. The counts live with the pushes, so no caller
/// can pop the wrong number.
void popMenuRowStyle();

/// Draws the hover band behind a whole row whose content starts at @p rowTopY,
/// so hovering reads as one row rather than as a control and a label lighting
/// up apart. The band reaches a little above and below the row and runs nearly
/// the popup's full width - just shy of the border, past the content into the
/// window padding - for a native-menu-style highlight edge to edge. Skipped
/// mid-drag, where the insertion bar is the cue.
void drawMenuRowHover(float rowTopY);

/// Draws @p key over the row just laid down, right-aligned @p rightPad in from
/// the row's edge and dimmed, the way a menu shows an accelerator. An empty
/// @p key draws nothing.
void drawMenuRowAccelerator(const char* key, float rightPad);

/// An icon button that stands in a menu row: the toolbar's glyph and hover
/// box, but only as tall as the row it shares, so a row carrying one is no
/// taller than a row of plain checkboxes and the two menus keep one rhythm.
/// @return Whether it was pressed.
[[nodiscard]] bool menuRowIconButton(const char* id, ImTextureID texture, const char* tooltip);

/// The width @ref menuRowIconButton takes, for a caller measuring its columns
/// before it draws them.
[[nodiscard]] float menuRowIconWidth();

}  // namespace sidescopes
