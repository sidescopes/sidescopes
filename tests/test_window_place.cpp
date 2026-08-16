#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "app/window_place.h"
#include "desktop_stubs.h"

namespace sidescopes {
namespace {

constexpr double StarterSquareHeightPercent = 34.0;

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

TEST_CASE("The first-run application window is compact and left-aligned")
{
    const WindowPlacement placement = starterWindowPlacement(WindowPlacement{100, -50, 1600, 900}, 340, 500);

    CHECK(placement.x == 132);
    CHECK(placement.y == 150);
    CHECK(placement.width == 340);
    CHECK(placement.height == 500);
}

TEST_CASE("The first-run application window fits a smaller work area")
{
    const WindowPlacement placement = starterWindowPlacement(WindowPlacement{-800, 0, 300, 420}, 340, 500);

    CHECK(placement.x == -800);
    CHECK(placement.y == 0);
    CHECK(placement.width == 300);
    CHECK(placement.height == 420);
}

TEST_CASE("The starter region uses the open side of the application window")
{
    const DisplayGeometry display = secondDisplay();
    const double squareWidthPercent = StarterSquareHeightPercent * display.heightPoints / display.widthPoints;

    SECTION("the application is near the right edge")
    {
        const WindowPlacement window{2100, 300, 440, 640};
        const RegionOfInterest region = starterGlobalRegion(window, display);
        const double appLeft = (window.x - display.originX) / display.widthPoints * 100.0;

        CHECK(region.rightPercent <= appLeft - 3.0 + 0.001);
        CHECK(region.leftPercent == Catch::Approx(50.0 - squareWidthPercent * 0.5));
        CHECK(region.topPercent == Catch::Approx(33.0));
        CHECK(region.rightPercent - region.leftPercent == Catch::Approx(squareWidthPercent));
        CHECK(region.bottomPercent - region.topPercent == Catch::Approx(34.0));
        CHECK((region.rightPercent - region.leftPercent) * display.widthPoints / 100.0 ==
              Catch::Approx((region.bottomPercent - region.topPercent) * display.heightPoints / 100.0));
    }

    SECTION("the application is near the left edge")
    {
        const WindowPlacement window{150, 300, 440, 640};
        const RegionOfInterest region = starterGlobalRegion(window, display);
        const double appRight = (window.x + window.width - display.originX) / display.widthPoints * 100.0;

        CHECK(region.leftPercent >= appRight + 3.0 - 0.001);
        CHECK(region.leftPercent == Catch::Approx(50.0 - squareWidthPercent * 0.5));
        CHECK(region.topPercent == Catch::Approx(33.0));
        CHECK(region.rightPercent - region.leftPercent == Catch::Approx(squareWidthPercent));
        CHECK(region.bottomPercent - region.topPercent == Catch::Approx(34.0));
        CHECK((region.rightPercent - region.leftPercent) * display.widthPoints / 100.0 ==
              Catch::Approx((region.bottomPercent - region.topPercent) * display.heightPoints / 100.0));
    }
}

TEST_CASE("The starter region remains valid without display geometry")
{
    const RegionOfInterest region = starterGlobalRegion(WindowPlacement{}, DisplayGeometry{});

    CHECK(region.leftPercent == 33.0);
    CHECK(region.topPercent == 33.0);
    CHECK(region.rightPercent == 67.0);
    CHECK(region.bottomPercent == 67.0);
}

TEST_CASE("The starter region moves away from a saved window in the centre")
{
    const DisplayGeometry display = secondDisplay();
    const WindowPlacement window{1050, 400, 500, 600};
    const RegionOfInterest region = starterGlobalRegion(window, display);
    const double windowLeft = (window.x - display.originX) / display.widthPoints * 100.0;
    const double windowTop = (window.y - display.originY) / display.heightPoints * 100.0;
    const double windowRight = (window.x + window.width - display.originX) / display.widthPoints * 100.0;
    const double windowBottom = (window.y + window.height - display.originY) / display.heightPoints * 100.0;
    const bool separated = region.rightPercent + 3.0 <= windowLeft || region.leftPercent >= windowRight + 3.0 ||
                           region.bottomPercent + 3.0 <= windowTop || region.topPercent >= windowBottom + 3.0;
    const double regionWidth = (region.rightPercent - region.leftPercent) * display.widthPoints / 100.0;
    const double regionHeight = (region.bottomPercent - region.topPercent) * display.heightPoints / 100.0;

    CHECK(separated);
    CHECK(regionWidth == Catch::Approx(regionHeight));
    const double centeredLeft = 50.0 - StarterSquareHeightPercent * display.heightPoints / display.widthPoints * 0.5;
    CHECK(region.leftPercent != Catch::Approx(centeredLeft));
}

TEST_CASE("The starter region remains square on a portrait display")
{
    DisplayGeometry display = secondDisplay();
    display.widthPoints = 900.0;
    display.heightPoints = 1600.0;
    const RegionOfInterest region = starterGlobalRegion(WindowPlacement{120, 300, 200, 500}, display);
    const double width = (region.rightPercent - region.leftPercent) * display.widthPoints / 100.0;
    const double height = (region.bottomPercent - region.topPercent) * display.heightPoints / 100.0;

    CHECK(width == Catch::Approx(height));
    CHECK(width == Catch::Approx(display.widthPoints * 0.36));
}

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
