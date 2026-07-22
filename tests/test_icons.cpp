#include <catch2/catch_test_macros.hpp>

#include "platform/icons.h"

namespace sidescopes {

namespace {

double alphaCoverage(const std::vector<uint8_t>& pixels)
{
    std::size_t covered = 0;
    for (std::size_t index = 3; index < pixels.size(); index += 4) {
        if (pixels[index] > 32) {
            ++covered;
        }
    }

    return static_cast<double>(covered) / (static_cast<double>(pixels.size()) / 4.0);
}

}  // namespace

TEST_CASE("Every icon rasterizes with plausible stroke coverage")
{
    for (const Icon icon :
         {Icon::Pin, Icon::PinOff, Icon::Paperclip, Icon::SquarePen, Icon::User, Icon::Pipette, Icon::Expand}) {
        for (const int size : {16, 24, 48}) {
            const auto pixels = rasterizeIcon(icon, size);
            REQUIRE(pixels.size() == static_cast<std::size_t>(size) * size * 4);
            // Strokes cover a modest slice of the square: an empty buffer
            // means a parse failure, a saturated one a fill gone wrong.
            const double coverage = alphaCoverage(pixels);
            CHECK(coverage > 0.03);
            CHECK(coverage < 0.6);
        }
    }
}

TEST_CASE("Icons are distinct images")
{
    // Named by glyph, like the enumerators: Pin and PinOff dress the
    // border's attach toggle, Paperclip the toolbar's attach button, and
    // SquarePen the toolbar's draw button.
    const auto pin = rasterizeIcon(Icon::Pin, 24);
    const auto pinOff = rasterizeIcon(Icon::PinOff, 24);
    const auto squarePen = rasterizeIcon(Icon::SquarePen, 24);
    const auto paperclip = rasterizeIcon(Icon::Paperclip, 24);
    CHECK(pin != pinOff);
    CHECK(pin != squarePen);
    CHECK(squarePen != paperclip);
}

TEST_CASE("The pin-off keeps its slash")
{
    // The diagonal strike-through is what tells the border toggle's global
    // state from its attached one; a rasterizer change must never lose it.
    const int size = 48;
    const auto pinOff = rasterizeIcon(Icon::PinOff, size);
    const auto pin = rasterizeIcon(Icon::Pin, size);
    REQUIRE(pinOff.size() == pin.size());
    const auto alphaAt = [](const std::vector<uint8_t>& pixels, double unitX, double unitY) {
        const int x = static_cast<int>(unitX / 24.0 * size);
        const int y = static_cast<int>(unitY / 24.0 * size);

        return pixels[(static_cast<std::size_t>(y) * size + x) * 4 + 3];
    };
    // The slash crosses 4,4 in pin-off; plain pin is clear there.
    CHECK(alphaAt(pinOff, 4.0, 4.0) > 32);
    CHECK(alphaAt(pin, 4.0, 4.0) <= 32);
}

}  // namespace sidescopes
