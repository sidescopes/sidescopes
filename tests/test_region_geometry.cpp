// Unit tests for the region picker's spatial math (region_geometry.cpp):
// the region <-> percentage conversion, the drag-selection rectangle, the
// grab zones, and the drag-resize clamping. The math is toolkit- and
// platform-independent, so these run on every platform and back both the
// Windows border procedure and the macOS overlay.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    const LocalRect region{0.0, 0.0, 100.0, 100.0};
    // 30 points from the corner: outside the 22-point zone at 1x, inside the
    // 44-point zone at 2x.
    CHECK(cornerZoneAt(region, 30.0, 30.0, 1.0) == ZoneNone);
    CHECK(cornerZoneAt(region, 30.0, 30.0, 2.0) == (ZoneLeft | ZoneTop));
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

}  // namespace sidescopes
