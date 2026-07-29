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
    for (const Icon icon : {Icon::Pin, Icon::PinOff, Icon::SquarePen, Icon::Pencil, Icon::User, Icon::Pipette,
                            Icon::SquareDashed, Icon::ChartColumn, Icon::PenLine, Icon::PanelsTopLeft, Icon::Save}) {
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
    // border's attach toggle, SquarePen the toolbar's attach button, and
    // Pencil the toolbar's draw button.
    const auto pin = rasterizeIcon(Icon::Pin, 24);
    const auto pinOff = rasterizeIcon(Icon::PinOff, 24);
    const auto squarePen = rasterizeIcon(Icon::SquarePen, 24);
    const auto pencil = rasterizeIcon(Icon::Pencil, 24);
    CHECK(pin != pinOff);
    CHECK(pin != squarePen);
    CHECK(squarePen != pencil);
    // PenLine renames a preset and Pencil draws a region - two edit-shaped
    // glyphs in one set, so the underline that tells them apart is worth
    // pinning rather than trusting to a glance.
    const auto penLine = rasterizeIcon(Icon::PenLine, 24);
    CHECK(penLine != pencil);
    CHECK(penLine != squarePen);
    // PanelsTopLeft opens the preset list and ChartColumn the scope list -
    // two buttons standing side by side on the toolbar, so the one thing
    // telling them apart is that they are not the same picture.
    const auto panels = rasterizeIcon(Icon::PanelsTopLeft, 24);
    const auto chartColumn = rasterizeIcon(Icon::ChartColumn, 24);
    CHECK(panels != chartColumn);
    CHECK(panels != squarePen);
    // The pen and the save stand side by side on every preset row, so the one
    // thing that has to hold is that they are not the same picture.
    const auto save = rasterizeIcon(Icon::Save, 24);
    CHECK(save != penLine);
    CHECK(save != panels);
    CHECK(save != squarePen);
}

TEST_CASE("The preset glyph is a frame divided into panes")
{
    // What makes it read as a layout rather than as a plain box: a rule across
    // it and another down from that rule. Losing either leaves a square, which
    // is what several other icons in the set already are.
    const int size = 48;
    const auto panels = rasterizeIcon(Icon::PanelsTopLeft, size);
    REQUIRE(panels.size() == static_cast<std::size_t>(size) * size * 4);
    const auto alphaAt = [&panels](double unitX, double unitY) {
        const int x = static_cast<int>(unitX / 24.0 * size);
        const int y = static_cast<int>(unitY / 24.0 * size);

        return panels[(static_cast<std::size_t>(y) * size + x) * 4 + 3];
    };
    // The horizontal rule crosses the middle of the frame at y = 9...
    CHECK(alphaAt(14.0, 9.0) > 32);
    // ...the vertical one runs below it at x = 9...
    CHECK(alphaAt(9.0, 15.0) > 32);
    // ...and the large pane they leave is empty.
    CHECK(alphaAt(15.0, 15.0) <= 32);
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
