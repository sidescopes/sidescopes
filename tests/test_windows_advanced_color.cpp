// The pure parts of the Windows advanced color glue: the fixed-point SDR white
// level DisplayConfig reports, and the defaults that keep an scRGB conversion
// neutral when the display configuration cannot be read. The live lookups
// need a desktop and are exercised by running the application.

#include <catch2/catch_test_macros.hpp>

#include "core/scrgb.h"
#include "platform/windows/advanced_color.h"

namespace sidescopes {

TEST_CASE("The SDR white level is fixed point over the scRGB white")
{
    CHECK(sdrWhiteNitsFromLevel(1000) == ScrgbWhiteNits);
    CHECK(sdrWhiteNitsFromLevel(3000) == 240.0);
    CHECK(sdrWhiteNitsFromLevel(1250) == 100.0);
    CHECK(sdrWhiteNitsFromLevel(0) == ScrgbWhiteNits);
}

TEST_CASE("An unknown colour target reads at the scRGB white")
{
    CHECK(sdrWhiteNits(ColorTarget{}) == ScrgbWhiteNits);
}

TEST_CASE("A device name no display carries resolves to no colour target")
{
    const ColorTarget target = findColorTarget(L"\\\\.\\DISPLAY4095");
    CHECK_FALSE(target.found);
    CHECK(sdrWhiteNits(target) == ScrgbWhiteNits);
}

}  // namespace sidescopes
