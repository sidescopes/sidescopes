#pragma once

#include <utility>
#include <vector>

namespace sidescopes {

/// How the enabled scopes divide the window. Automatic keeps the historical
/// behavior - the longer window axis splits - while Vertical and Horizontal
/// pin the split direction regardless of the window's proportions.
enum class LayoutOrientation
{
    Automatic,   ///< Split along the longer axis, as the app always has.
    Vertical,    ///< Panes stacked top to bottom.
    Horizontal,  ///< Panes side by side, left to right.
};

/// The concrete split direction after Automatic has been resolved against the
/// pane area.
enum class SplitDirection
{
    SideBySide,  ///< Panes run left to right; the split axis is X.
    Stacked,     ///< Panes run top to bottom; the split axis is Y.
};

/// The smallest a pane may shrink to before the divider stops, in 100%-scale
/// points; the host multiplies it by the UI scale.
inline constexpr float MinPaneLength = 80.0f;

/// The grabbable divider's thickness between two panes, in 100%-scale points.
inline constexpr float DividerThickness = 6.0f;

/// Resolves @p orientation for a pane area of @p areaWidth by @p areaHeight.
/// Automatic picks the longer axis (ties split side by side, matching the
/// historical default); Vertical and Horizontal ignore the area.
[[nodiscard]] SplitDirection resolveSplitDirection(LayoutOrientation orientation, float areaWidth, float areaHeight);

/// Distributes @p totalLength (the axis length already minus the inter-pane
/// dividers) across panes in proportion to @p weights, clamping each pane to at
/// least @p minLength and rebalancing so the lengths still sum to
/// @p totalLength. When the area cannot hold every pane at @p minLength, the
/// panes split evenly and accept the sub-minimum rather than overflow. Returns
/// one length per weight, in order.
[[nodiscard]] std::vector<float> paneLengths(const std::vector<float>& weights, float totalLength, float minLength);

/// Re-splits the combined weight of two adjacent panes after a divider drag of
/// @p deltaPixels, given their current pixel lengths. Both panes keep at least
/// @p minLength, and their combined weight is preserved so the other panes are
/// untouched. Returns the two new weights, first then second.
[[nodiscard]] std::pair<float, float> dragDividerWeights(float weightFirst, float weightSecond, float lengthFirst,
                                                         float lengthSecond, float deltaPixels, float minLength);

/// Maps a persisted integer to an orientation: 1 Vertical, 2 Horizontal, any
/// other value (including the default 0) Automatic.
[[nodiscard]] LayoutOrientation orientationFromInt(int value);

/// The persisted integer for @p orientation, the inverse of orientationFromInt.
[[nodiscard]] int orientationToInt(LayoutOrientation orientation);

}  // namespace sidescopes
