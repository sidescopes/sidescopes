#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "core/analysis_worker.h"  // RegionOfInterest

namespace sidescopes {

/// Which edges a border drag adjusts; Move relocates the whole region,
/// Close dismisses it. Shared by the geometry helpers below and each
/// platform's border overlay.
enum ZoneBits : unsigned
{
    ZoneNone = 0,
    ZoneLeft = 1u << 0,
    ZoneRight = 1u << 1,
    ZoneTop = 1u << 2,
    ZoneBottom = 1u << 3,
    ZoneMove = 1u << 4,
    ZoneClose = 1u << 5,
    ZoneAttach = 1u << 6,
};

/// A rectangle in overlay-local points. The region math is expressed
/// through this toolkit-independent type - not a native rectangle type -
/// so it carries no windowing dependency and can be reasoned about and
/// unit-tested on its own.
struct LocalRect
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/// Overlay-local points to display-relative percentages, and back. The
/// percentages let a selection survive capture-resolution changes.
RegionOfInterest regionFromLocalRect(const LocalRect& rect, double width, double height);
LocalRect localRectFromRegion(const RegionOfInterest& region, double width, double height);

/// The drag rectangle between two pointer positions, normalized so it reads
/// the same whichever way the drag was made.
LocalRect selectionRectFromDrag(double startX, double startY, double currentX, double currentY);

/// The grab zone for a point already known to be outside the region.
/// cornerZoneAt returns a two-axis corner zone, or ZoneNone when the point
/// is not in a corner. edgeOrMoveZoneAt returns a one-axis edge zone at an
/// edge's midpoint, or ZoneMove anywhere else on the band.
unsigned cornerZoneAt(const LocalRect& region, double x, double y, double scale);
unsigned edgeOrMoveZoneAt(const LocalRect& region, double x, double y, double scale);

/// A drag delta applied to a region, clamped so no dragged edge crosses
/// within the minimum of its opposite. Move offsets the whole rectangle; an
/// edge or corner zone moves only those sides.
LocalRect draggedRegionRect(unsigned dragZone, const LocalRect& start, double dx, double dy, double minimum);

/// The least share of itself a window must still show to be suggested as a
/// pick. At 0.15 a window peeking out from behind the editor still counts,
/// while one hidden down to a sliver drops out: low enough not to lose a
/// genuinely reachable window, high enough that a click meant for it would
/// not as readily land on whatever covers it.
inline constexpr double MinimumVisibleFraction = 0.15;

/// The visible fraction of each window: the share of its area not hidden under
/// the union of the windows in front of it. @p windows is front-to-back
/// (frontmost first), matching the window server's order, so window i is
/// covered only by windows [0, i). A degenerate (zero-area) window reports 0;
/// every fraction lies in [0, 1].
/// @param windows the on-screen windows, frontmost first.
/// @return one visible fraction per window, in the same order.
[[nodiscard]] std::vector<double> visibleFractions(const std::vector<LocalRect>& windows);

/// The frontmost window whose unoccluded region contains the point. @p windows
/// is front-to-back; the first window that contains the point wins, since
/// nothing in front of it covers that point.
/// @param windows the on-screen windows, frontmost first.
/// @return the index of the topmost visible window at the point, or no value
/// when the point lies over bare desktop or only over hidden parts of windows.
[[nodiscard]] std::optional<std::size_t> topmostVisibleWindowAt(const std::vector<LocalRect>& windows, double x,
                                                                double y);

/// The windows worth suggesting as picks: those at least
/// MinimumVisibleFraction visible. Fully hidden windows never appear, so the
/// picker stops proposing windows the user cannot see behind the ones on top.
/// @param windows the on-screen windows, frontmost first.
/// @return candidate indices into @p windows, frontmost first.
[[nodiscard]] std::vector<std::size_t> meaningfulPickCandidates(const std::vector<LocalRect>& windows);

/// The label tab's width budget on the border's strip row: the pin zone
/// leads, the text follows with @p textPad on both sides, and a region
/// too small for legible text shows no tab at all. Both platforms lay the
/// tab out through this one function, so the degradation ladder on
/// shrinking regions stays identical - and tested.
struct TabLayout
{
    bool visible = false;
    double textWidth = 0.0;
    double tabWidth = 0.0;
};

/// @param availableWidth the room from the tab's left edge to the border
///        surface's usable right edge.
/// @param pinZone the attach toggle's reserved lead width.
/// @param textPad the horizontal padding on each side of the text.
/// @param measuredText the label's natural width.
/// @param minimumText the narrowest text still worth showing.
[[nodiscard]] TabLayout borderTabLayout(double availableWidth, double pinZone, double textPad, double measuredText,
                                        double minimumText);

}  // namespace sidescopes
