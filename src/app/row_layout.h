#pragma once

#include "imgui.h"

/// Seating rules for a row that mixes icons, swatches and text.
///
/// A line box is taller than the ink inside it, and an icon's box is taller
/// again, so "put these side by side" is arithmetic rather than a guess. These
/// helpers hold that arithmetic in one place, where the layout tests can reach
/// it: a rule that only exists inside a drawing function can be asserted only
/// by copying it, and a copy proves nothing about what ships.
namespace sidescopes {

/// The width of an icon button's box: the glyph plus a margin each side.
[[nodiscard]] float iconButtonWidth();

/// The height of an icon button's box, the tallest thing a row can carry.
[[nodiscard]] float iconButtonHeight();

/// The width of an icon button carrying @p labelWidth of text beside its
/// glyph: the plain button's box, then the label, then the same margin the
/// glyph keeps on its left, so the ink is inset equally at both ends.
///
/// @p labelWidth is what the WIDEST label the button will ever show measures,
/// not the one it is showing. A box that grew and shrank with its own text
/// would move everything to its right, and the toolbox beyond that reflows on
/// a threshold - the "it dropped to a second row by itself" defect.
[[nodiscard]] float labelledIconButtonWidth(float labelWidth);

/// How far the glyph sits inside its button's box - the left margin a line of
/// text needs to start where an icon in that box appears to start.
[[nodiscard]] float iconButtonInset();

/// The glyph's top-left corner inside the button box spanning @p min to @p max,
/// for a square glyph of @p side.
///
/// Measured from the box's own edges, never from its centre on screen: a box an
/// odd number of pixels taller than its glyph puts that centre on a half pixel,
/// and rounding it drifts with wherever the window sits. From the edge the
/// glyph lands on the same rows as a swatch or a line of text beside it, at
/// every window position.
[[nodiscard]] ImVec2 iconGlyphOrigin(const ImVec2& min, const ImVec2& max, float side);

/// How far a text line or swatch drops to share the centre line of the icon
/// button standing beside it.
[[nodiscard]] float rowTextDrop();

/// The columns of a channel readout: a letter, its value, and the gaps that
/// bind each value to its own letter.
///
/// Values are left-aligned at fixed positions, so no digit coming or going
/// moves anything; the slack a short reading leaves falls BETWEEN groups, where
/// it reinforces the grouping instead of loosening it.
struct ReadoutColumns
{
    /// Width of the channel letter's column.
    float label;
    /// One space, the gap between a letter and its value.
    float gap;
    /// Distance from one group's start to the next.
    float stride;
    /// Total width of the three groups, without the trailing separation.
    float width;
};

/// Measures the readout columns against the current font.
[[nodiscard]] ReadoutColumns measureReadoutColumns();

/// Vertical centre of the visible ink used by the RGB readout, measured from
/// the text line's top. Font line boxes reserve ascent/descent space that is
/// not visually balanced around capital letters, digits and `%`; a swatch
/// centred on the box therefore sits high or low beside the glyphs. This
/// answers the real ink centre for the current font, size and raster density.
[[nodiscard]] float readoutTextInkCenter();

/// How far the status row drops to sit centred in the strip.
///
/// The strip is bounded above by the spacing parting it from the panes and
/// below by the window's own padding; where those differ, a row centred in its
/// own height still lands off centre in the visible gap.
[[nodiscard]] float statusRowOffset();

/// The gap a row keeps between neighbours that are not part of one reading.
inline constexpr float RowSeparation = 8.0f;

/// Whether the region toolbox of @p toolboxWidth drops to a row of its own,
/// rather than sitting right-aligned on the @p rowWidth row it shares, with
/// @p taken of that row already standing to its left.
///
/// The toolbox is a constant-width cluster - state dims a tool, it never
/// removes one - so the row reflows only when the window changes. Anything
/// that comes and goes to its left reflows it too, which is why a message
/// that shows for a moment belongs on the status bar and not on this row.
[[nodiscard]] bool regionToolboxWraps(float taken, float toolboxWidth, float rowWidth);

}  // namespace sidescopes
