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
/// what a control that shows only under the pointer asks before painting
/// itself. It is the WHOLE row that answers, not the control, so a pen the
/// pointer has not reached yet still appears when its row is entered.
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

/// An icon button that stands in a menu row: the toolbar's glyph and hover
/// box, but only as tall as the row it shares, so a row carrying one is no
/// taller than a row of plain checkboxes and the two menus keep one rhythm.
///
/// @p painted false draws nothing and STILL TAKES THE SAME BOX, which is what
/// a control that appears only under the pointer needs: reserving the space
/// only while it shows would step every name in the list sideways as the
/// pointer moves down it. Pass @ref menuRowHovered for that row.
/// @return Whether it was pressed.
[[nodiscard]] bool menuRowIconButton(const char* id, ImTextureID texture, const char* tooltip, bool painted = true);

/// The width @ref menuRowIconButton takes, for a caller measuring its columns
/// before it draws them.
[[nodiscard]] float menuRowIconWidth();

}  // namespace sidescopes
