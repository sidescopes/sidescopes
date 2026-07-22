// Unit tests for the region picker's spatial math (region_geometry.cpp):
// the region <-> percentage conversion, the drag-selection rectangle, the
// grab zones, and the drag-resize clamping. The math is toolkit- and
// platform-independent, so these run on every platform and back both the
// Windows border procedure and the macOS overlay.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <vector>

#include "platform/region_geometry.h"

namespace sidescopes {
namespace {

using Catch::Approx;

}  // namespace

// ---------------------------------------------------------------------------
// Region <-> percentage conversion
// ---------------------------------------------------------------------------

TEST_CASE("A full-size local rect maps to the whole region")
{
    const RegionOfInterest region = regionFromLocalRect(LocalRect{0.0, 0.0, 100.0, 50.0}, 100.0, 50.0);
    CHECK(region.leftPercent == Approx(0.0));
    CHECK(region.topPercent == Approx(0.0));
    CHECK(region.rightPercent == Approx(100.0));
    CHECK(region.bottomPercent == Approx(100.0));
}

TEST_CASE("A local rect maps to percentages of its display")
{
    const RegionOfInterest region = regionFromLocalRect(LocalRect{25.0, 10.0, 50.0, 30.0}, 100.0, 100.0);
    CHECK(region.leftPercent == Approx(25.0));
    CHECK(region.topPercent == Approx(10.0));
    CHECK(region.rightPercent == Approx(75.0));
    CHECK(region.bottomPercent == Approx(40.0));
}

TEST_CASE("A region maps back to a local rect on its display")
{
    const LocalRect rect = localRectFromRegion(RegionOfInterest{25.0, 10.0, 75.0, 40.0}, 200.0, 100.0);
    CHECK(rect.x == Approx(50.0));
    CHECK(rect.y == Approx(10.0));
    CHECK(rect.width == Approx(100.0));
    CHECK(rect.height == Approx(30.0));
}

TEST_CASE("Local rect and region conversions round-trip")
{
    const LocalRect original{30.0, 20.0, 40.0, 25.0};
    const RegionOfInterest region = regionFromLocalRect(original, 200.0, 150.0);
    const LocalRect restored = localRectFromRegion(region, 200.0, 150.0);
    CHECK(restored.x == Approx(original.x));
    CHECK(restored.y == Approx(original.y));
    CHECK(restored.width == Approx(original.width));
    CHECK(restored.height == Approx(original.height));
}

// ---------------------------------------------------------------------------
// Drag-selection rectangle
// ---------------------------------------------------------------------------

TEST_CASE("A selection drag reads the same in either direction")
{
    const LocalRect forward = selectionRectFromDrag(10.0, 20.0, 60.0, 80.0);
    const LocalRect backward = selectionRectFromDrag(60.0, 80.0, 10.0, 20.0);
    for (const LocalRect& rect : {forward, backward}) {
        CHECK(rect.x == Approx(10.0));
        CHECK(rect.y == Approx(20.0));
        CHECK(rect.width == Approx(50.0));
        CHECK(rect.height == Approx(60.0));
    }
}

TEST_CASE("A selection drag that never moved is empty")
{
    const LocalRect rect = selectionRectFromDrag(30.0, 30.0, 30.0, 30.0);
    CHECK(rect.width == Approx(0.0));
    CHECK(rect.height == Approx(0.0));
}

// ---------------------------------------------------------------------------
// Corner grab zones
// ---------------------------------------------------------------------------

TEST_CASE("Each corner grabs both of its axes")
{
    const LocalRect region{0.0, 0.0, 100.0, 100.0};
    CHECK(cornerZoneAt(region, 5.0, 5.0, 1.0) == (ZoneLeft | ZoneTop));
    CHECK(cornerZoneAt(region, 95.0, 5.0, 1.0) == (ZoneRight | ZoneTop));
    CHECK(cornerZoneAt(region, 5.0, 95.0, 1.0) == (ZoneLeft | ZoneBottom));
    CHECK(cornerZoneAt(region, 95.0, 95.0, 1.0) == (ZoneRight | ZoneBottom));
}

TEST_CASE("A point away from every corner is not a corner grab")
{
    const LocalRect region{0.0, 0.0, 100.0, 100.0};
    CHECK(cornerZoneAt(region, 50.0, 5.0, 1.0) == ZoneNone);   // top edge, mid-span
    CHECK(cornerZoneAt(region, 50.0, 50.0, 1.0) == ZoneNone);  // interior
}

TEST_CASE("The scale factor grows the corner zone")
{
    // Large enough that the quarter-side cap stays out of the way: the zone
    // is purely the scaled 22 points.
    const LocalRect region{0.0, 0.0, 200.0, 200.0};
    // 30 points from the corner: outside the 22-point zone at 1x, inside the
    // 44-point zone at 2x.
    CHECK(cornerZoneAt(region, 30.0, 30.0, 1.0) == ZoneNone);
    CHECK(cornerZoneAt(region, 30.0, 30.0, 2.0) == (ZoneLeft | ZoneTop));
}

TEST_CASE("A small region keeps a grabbable move band between the zones")
{
    // 40x40: uncapped 22-point corners would cover every band point twice
    // over. Capped to a sixth per side, the corners reach about 6.7 points
    // and the edge midpoints span about 3.3 to either side of center,
    // leaving roughly half of every edge as move band.
    const LocalRect region{100.0, 100.0, 40.0, 40.0};

    CHECK(cornerZoneAt(region, 105.0, 105.0, 1.0) == (ZoneLeft | ZoneTop));
    CHECK(cornerZoneAt(region, 110.0, 110.0, 1.0) == ZoneNone);     // past the capped corner
    CHECK(edgeOrMoveZoneAt(region, 120.0, 95.0, 1.0) == ZoneTop);   // edge midpoint above
    CHECK(edgeOrMoveZoneAt(region, 113.0, 95.0, 1.0) == ZoneMove);  // the gap corner-to-midpoint
    CHECK(edgeOrMoveZoneAt(region, 127.0, 95.0, 1.0) == ZoneMove);  // the gap midpoint-to-corner
}

// ---------------------------------------------------------------------------
// Edge and move grab zones
// ---------------------------------------------------------------------------

TEST_CASE("Each edge midpoint grabs its own edge")
{
    const LocalRect region{0.0, 0.0, 100.0, 100.0};
    CHECK(edgeOrMoveZoneAt(region, 50.0, -5.0, 1.0) == ZoneTop);
    CHECK(edgeOrMoveZoneAt(region, 50.0, 105.0, 1.0) == ZoneBottom);
    CHECK(edgeOrMoveZoneAt(region, -5.0, 50.0, 1.0) == ZoneLeft);
    CHECK(edgeOrMoveZoneAt(region, 105.0, 50.0, 1.0) == ZoneRight);
}

TEST_CASE("The band away from any midpoint moves the whole region")
{
    const LocalRect region{0.0, 0.0, 100.0, 100.0};
    CHECK(edgeOrMoveZoneAt(region, -5.0, -5.0, 1.0) == ZoneMove);
    CHECK(edgeOrMoveZoneAt(region, -5.0, 10.0, 1.0) == ZoneMove);
}

// ---------------------------------------------------------------------------
// Drag-resize clamping
// ---------------------------------------------------------------------------

TEST_CASE("Moving the region shifts it without resizing")
{
    const LocalRect moved = draggedRegionRect(ZoneMove, LocalRect{100.0, 100.0, 200.0, 150.0}, 10.0, 20.0, 24.0);
    CHECK(moved.x == Approx(110.0));
    CHECK(moved.y == Approx(120.0));
    CHECK(moved.width == Approx(200.0));
    CHECK(moved.height == Approx(150.0));
}

TEST_CASE("Dragging one edge moves only that edge")
{
    const LocalRect dragged = draggedRegionRect(ZoneLeft, LocalRect{100.0, 100.0, 200.0, 150.0}, 30.0, 0.0, 24.0);
    CHECK(dragged.x == Approx(130.0));
    CHECK(dragged.width == Approx(170.0));
    CHECK(dragged.y == Approx(100.0));
    CHECK(dragged.height == Approx(150.0));
}

TEST_CASE("A dragged edge cannot cross within the minimum of its opposite")
{
    const LocalRect start{100.0, 100.0, 200.0, 150.0};
    // Left edge shoved far past the right: it stops one minimum short.
    const LocalRect left = draggedRegionRect(ZoneLeft, start, 500.0, 0.0, 24.0);
    CHECK(left.x == Approx(276.0));
    CHECK(left.width == Approx(24.0));
    // Right edge shoved far past the left: same floor from the other side.
    const LocalRect right = draggedRegionRect(ZoneRight, start, -500.0, 0.0, 24.0);
    CHECK(right.x == Approx(100.0));
    CHECK(right.width == Approx(24.0));
}

TEST_CASE("Dragging a corner moves both of its edges")
{
    const LocalRect dragged =
        draggedRegionRect(ZoneLeft | ZoneTop, LocalRect{100.0, 100.0, 200.0, 150.0}, 10.0, 15.0, 24.0);
    CHECK(dragged.x == Approx(110.0));
    CHECK(dragged.y == Approx(115.0));
    CHECK(dragged.width == Approx(190.0));
    CHECK(dragged.height == Approx(135.0));
}

// ---------------------------------------------------------------------------
// Window occlusion: visible fractions, topmost-at-point, pick candidates.
// Windows are front-to-back - index 0 is frontmost - so a window is hidden by
// the ones with lower indices.
// ---------------------------------------------------------------------------

TEST_CASE("A lone window is fully visible and a pick candidate")
{
    const std::vector<LocalRect> windows{{0.0, 0.0, 100.0, 100.0}};
    const std::vector<double> fractions = visibleFractions(windows);
    REQUIRE(fractions.size() == 1);
    CHECK(fractions[0] == Approx(1.0));
    CHECK(meaningfulPickCandidates(windows) == std::vector<std::size_t>{0});
    CHECK(topmostVisibleWindowAt(windows, 50.0, 50.0) == std::optional<std::size_t>{0});
    CHECK_FALSE(topmostVisibleWindowAt(windows, 150.0, 50.0).has_value());
}

TEST_CASE("A window fully behind another shows nothing")
{
    // Two identical rectangles: the exact-overlap case. The back one (index 1)
    // is completely buried by the front one (index 0).
    const std::vector<LocalRect> windows{{0.0, 0.0, 100.0, 100.0}, {0.0, 0.0, 100.0, 100.0}};
    const std::vector<double> fractions = visibleFractions(windows);
    CHECK(fractions[0] == Approx(1.0));
    CHECK(fractions[1] == Approx(0.0));
    CHECK(meaningfulPickCandidates(windows) == std::vector<std::size_t>{0});
    // The shared area belongs to the front window.
    CHECK(topmostVisibleWindowAt(windows, 50.0, 50.0) == std::optional<std::size_t>{0});
}

TEST_CASE("A partly covered window keeps its exposed fraction")
{
    // The front window (index 0) covers the top-left quarter of the back one.
    const std::vector<LocalRect> windows{{0.0, 0.0, 50.0, 50.0}, {0.0, 0.0, 100.0, 100.0}};
    const std::vector<double> fractions = visibleFractions(windows);
    CHECK(fractions[0] == Approx(1.0));
    CHECK(fractions[1] == Approx(0.75));
    CHECK(meaningfulPickCandidates(windows) == (std::vector<std::size_t>{0, 1}));
}

TEST_CASE("Coverage counts the union of the windows in front")
{
    // Two front windows each hide one half of the back window; neither hides
    // it alone, but together they bury it. Testing coverage against a single
    // front window at a time - the flaw this geometry replaces - would leave
    // the buried window a candidate.
    const std::vector<LocalRect> windows{{0.0, 0.0, 50.0, 100.0},    // left half, frontmost
                                         {50.0, 0.0, 50.0, 100.0},   // right half
                                         {0.0, 0.0, 100.0, 100.0}};  // fully behind both
    const std::vector<double> fractions = visibleFractions(windows);
    CHECK(fractions[2] == Approx(0.0));
    CHECK(meaningfulPickCandidates(windows) == (std::vector<std::size_t>{0, 1}));
}

TEST_CASE("A point resolves to the frontmost window that shows there")
{
    // Back window (index 1) with the front window (index 0) over its left half.
    const std::vector<LocalRect> windows{{0.0, 0.0, 50.0, 100.0}, {0.0, 0.0, 100.0, 100.0}};
    // Left half: the front window is on top there.
    CHECK(topmostVisibleWindowAt(windows, 25.0, 50.0) == std::optional<std::size_t>{0});
    // Right half: only the back window reaches it.
    CHECK(topmostVisibleWindowAt(windows, 75.0, 50.0) == std::optional<std::size_t>{1});
    // Off every window.
    CHECK_FALSE(topmostVisibleWindowAt(windows, 150.0, 150.0).has_value());
}

TEST_CASE("The visibility threshold includes a window right at the boundary")
{
    // An 85-wide strip over a 100x100 window leaves exactly the 0.15 threshold.
    const std::vector<LocalRect> atThreshold{{0.0, 0.0, 85.0, 100.0}, {0.0, 0.0, 100.0, 100.0}};
    CHECK(visibleFractions(atThreshold)[1] == Approx(MinimumVisibleFraction));
    CHECK(meaningfulPickCandidates(atThreshold) == (std::vector<std::size_t>{0, 1}));

    // A 90-wide strip leaves 0.10 - below the threshold, so it drops out.
    const std::vector<LocalRect> belowThreshold{{0.0, 0.0, 90.0, 100.0}, {0.0, 0.0, 100.0, 100.0}};
    CHECK(visibleFractions(belowThreshold)[1] == Approx(0.10));
    CHECK(meaningfulPickCandidates(belowThreshold) == std::vector<std::size_t>{0});
}

TEST_CASE("A cascade of windows each keeps its exposed corner")
{
    // Four windows stepping down-right, each overlapping the next by a quarter.
    const std::vector<LocalRect> windows{{0.0, 0.0, 100.0, 100.0},
                                         {50.0, 50.0, 100.0, 100.0},
                                         {100.0, 100.0, 100.0, 100.0},
                                         {150.0, 150.0, 100.0, 100.0}};
    const std::vector<double> fractions = visibleFractions(windows);
    CHECK(fractions[0] == Approx(1.0));   // frontmost, nothing above it
    CHECK(fractions[1] == Approx(0.75));  // its top-left quarter under [0]
    CHECK(fractions[2] == Approx(0.75));  // only [1] reaches it
    CHECK(fractions[3] == Approx(0.75));  // only [2] reaches it
    CHECK(meaningfulPickCandidates(windows).size() == 4);
}

TEST_CASE("Zero-area windows are neither visible nor occluding")
{
    // A degenerate window is never a candidate and holds no point.
    const std::vector<LocalRect> degenerate{{10.0, 10.0, 0.0, 0.0}};
    CHECK(visibleFractions(degenerate)[0] == Approx(0.0));
    CHECK(meaningfulPickCandidates(degenerate).empty());
    CHECK_FALSE(topmostVisibleWindowAt(degenerate, 10.0, 10.0).has_value());

    // A degenerate window in front hides nothing behind it.
    const std::vector<LocalRect> withDegenerateFront{{0.0, 0.0, 0.0, 100.0}, {0.0, 0.0, 100.0, 100.0}};
    CHECK(visibleFractions(withDegenerateFront)[1] == Approx(1.0));
    CHECK(meaningfulPickCandidates(withDegenerateFront) == std::vector<std::size_t>{1});
}

TEST_CASE("The label tab steps down gracefully as the region narrows")
{
    // Wide: the text fits whole.
    const TabLayout wide = borderTabLayout(400.0, 18.0, 6.0, 120.0, 16.0);
    CHECK(wide.visible);
    CHECK(wide.textWidth == 120.0);
    CHECK(wide.tabWidth == 18.0 + 120.0 + 12.0);

    // Narrower: the text truncates to the budget.
    const TabLayout narrow = borderTabLayout(100.0, 18.0, 6.0, 120.0, 16.0);
    CHECK(narrow.visible);
    CHECK(narrow.textWidth == 100.0 - 18.0 - 12.0);
    CHECK(narrow.tabWidth == 100.0);

    // Too small for legible text: no tab at all.
    const TabLayout tiny = borderTabLayout(40.0, 18.0, 6.0, 120.0, 16.0);
    CHECK_FALSE(tiny.visible);
    CHECK(tiny.tabWidth == 0.0);
}

TEST_CASE("The label tab never exceeds its budget")
{
    for (int step = 0; step <= 300; step += 7) {
        const double available = static_cast<double>(step);
        const TabLayout layout = borderTabLayout(available, 18.0, 6.0, 500.0, 16.0);
        if (layout.visible) {
            CHECK(layout.tabWidth <= available);
            CHECK(layout.textWidth >= 16.0);
        }
    }
}

}  // namespace sidescopes
