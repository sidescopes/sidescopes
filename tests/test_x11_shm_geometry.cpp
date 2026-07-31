#include <catch2/catch_test_macros.hpp>

#include "platform/linux/x11_shm_geometry.h"

using namespace sidescopes;

// The XShm crop-and-stamp arithmetic, pinned without an X server. A coordinate
// mistake here is the shape that has twice been a shipped defect on the other
// platforms: a frame that reads the wrong pixels, or resolves a region against
// the wrong extents. The stamp contract is FrameView's: all-zero source fields
// mean the frame covers the whole display; a crop stamps its own origin with
// the DISPLAY's extents, never the crop's.

TEST_CASE("a whole-display grab reads the display rect and stamps all-zero source")
{
    // A second monitor, offset on the root.
    const DisplayGeometry display{100.0, 50.0, 1920.0, 1080.0};
    const GrabRect grab = computeGrab(display, std::nullopt);
    CHECK(grab.rootX == 100);
    CHECK(grab.rootY == 50);
    CHECK(grab.width == 1920);
    CHECK(grab.height == 1080);
    CHECK(grab.sourceX == 0);
    CHECK(grab.sourceY == 0);
    CHECK(grab.sourceWidth == 0);
    CHECK(grab.sourceHeight == 0);
}

TEST_CASE("a crop grabs at the display origin plus the crop and stamps the display extents")
{
    const DisplayGeometry display{100.0, 50.0, 1920.0, 1080.0};
    const GrabRect grab = computeGrab(display, IntRect{200, 100, 640, 480});
    // Root read offset is the display origin plus the crop's own origin.
    CHECK(grab.rootX == 300);
    CHECK(grab.rootY == 150);
    CHECK(grab.width == 640);
    CHECK(grab.height == 480);
    // The stamp carries the crop origin (display-relative) with the WHOLE
    // display's extents, so a percentage region resolves against the display.
    CHECK(grab.sourceX == 200);
    CHECK(grab.sourceY == 100);
    CHECK(grab.sourceWidth == 1920);
    CHECK(grab.sourceHeight == 1080);
}

TEST_CASE("a crop running off the display edge is clamped inside it")
{
    const DisplayGeometry display{0.0, 0.0, 800.0, 600.0};
    const GrabRect grab = computeGrab(display, IntRect{700, 500, 400, 400});
    CHECK(grab.rootX == 700);
    CHECK(grab.rootY == 500);
    CHECK(grab.width == 100);
    CHECK(grab.height == 100);
    // Never asks the server for pixels past the root.
    CHECK(grab.rootX + grab.width <= 800);
    CHECK(grab.rootY + grab.height <= 600);
}

TEST_CASE("a crop origin beyond the display is clamped and keeps a one-pixel floor")
{
    const DisplayGeometry display{0.0, 0.0, 800.0, 600.0};
    const GrabRect grab = computeGrab(display, IntRect{900, 700, 50, 50});
    CHECK(grab.width >= 1);
    CHECK(grab.height >= 1);
    CHECK(grab.rootX + grab.width <= 800);
    CHECK(grab.rootY + grab.height <= 600);
}
