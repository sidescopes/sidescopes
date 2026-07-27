#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "app/window_place.h"
#include "desktop_stubs.h"

namespace sidescopes {
namespace {

// A 2560x1440 display whose top-left sits 100 points right and 50 down of the
// desktop origin, delivered at its own pixel extents.
DisplayGeometry secondDisplay()
{
    DisplayGeometry display;
    display.originX = 100.0;
    display.originY = 50.0;
    display.widthPoints = 2560.0;
    display.heightPoints = 1440.0;

    return display;
}

}  // namespace

TEST_CASE("A window belongs to the display under its centre")
{
    test::desktopStubs().reset();
    test::desktopStubs().cursorDisplay = 7;

    // The literal is unsigned: comparing an optional<uint32_t> against a plain
    // one is a signed/unsigned mismatch MSVC rejects under its warning wall.
    CHECK(displayUnderWindow(WindowPlacement{200, 100, 400, 300}) == uint32_t{7});
    REQUIRE(test::desktopStubs().lastDisplayPoint);
    CHECK(test::desktopStubs().lastDisplayPoint->x == 400.0);
    CHECK(test::desktopStubs().lastDisplayPoint->y == 250.0);
}

TEST_CASE("The self mask is the window plus its chrome, in display pixels")
{
    // One pixel per point: the mask is the window grown by the margins.
    const IntRect plain = selfWindowMask(WindowPlacement{300, 250, 400, 300}, secondDisplay(), 2560, 1440, 1.0f);
    CHECK(plain.x == 300 - 100 - 8);
    CHECK(plain.y == 250 - 50 - 42);
    CHECK(plain.width == 400 + 16);
    CHECK(plain.height == 300 + 58);
}

TEST_CASE("The self mask is stated in display pixels, not window points")
{
    // The same window on a display delivering two pixels per point: every
    // number doubles, which is what makes the mask survive a narrowed capture.
    const IntRect retina = selfWindowMask(WindowPlacement{300, 250, 400, 300}, secondDisplay(), 5120, 2880, 1.0f);
    CHECK(retina.x == (300 - 100 - 8) * 2);
    CHECK(retina.y == (250 - 50 - 42) * 2);
    CHECK(retina.width == (400 + 16) * 2);
    CHECK(retina.height == (300 + 58) * 2);
}

TEST_CASE("The chrome margins grow with the interface scale")
{
    const IntRect plain = selfWindowMask(WindowPlacement{300, 250, 400, 300}, secondDisplay(), 2560, 1440, 1.0f);
    const IntRect scaled = selfWindowMask(WindowPlacement{300, 250, 400, 300}, secondDisplay(), 2560, 1440, 2.0f);

    // The window keeps its size; only the allowance around it doubles.
    CHECK(scaled.x == plain.x - 8);
    CHECK(scaled.y == plain.y - 42);
    CHECK(scaled.width == plain.width + 16);
    CHECK(scaled.height == plain.height + 58);
}

}  // namespace sidescopes
