#include <catch2/catch_test_macros.hpp>

#include "web/lab_layout.h"

namespace sidescopes {
namespace {

[[nodiscard]] bool overlaps(const ShellLayout& layout)
{
    const float screenRight = layout.screenPos.x + layout.screenSize.x;
    const float screenBottom = layout.screenPos.y + layout.screenSize.y;
    const float appRight = layout.appPos.x + layout.appSize.x;
    const float appBottom = layout.appPos.y + layout.appSize.y;

    return layout.appPos.x < screenRight && appRight > layout.screenPos.x && layout.appPos.y < screenBottom &&
           appBottom > layout.screenPos.y;
}

}  // namespace

TEST_CASE("On a wide viewport the application sits beside the picture")
{
    const ShellLayout layout = layoutFor(LayoutPoint{0.0f, 0.0f}, LayoutPoint{1400.0f, 800.0f});

    CHECK(layout.appPos.x > layout.screenPos.x + layout.screenSize.x - 1.0f);
    CHECK(layout.screenSize.y == 800.0f);
}

TEST_CASE("On a tall viewport the application sits below the picture")
{
    const ShellLayout layout = layoutFor(LayoutPoint{0.0f, 0.0f}, LayoutPoint{600.0f, 1200.0f});

    CHECK(layout.appPos.y > layout.screenPos.y + layout.screenSize.y - 1.0f);
    CHECK(layout.screenSize.x == 600.0f);
}

TEST_CASE("The application is never a frame around the picture")
{
    // The one thing this layout would teach falsely if it were wrong. On a
    // desktop the two are separate windows; a lab that nested one inside the
    // other would be showing an arrangement that does not exist.
    for (const LayoutPoint size :
         {LayoutPoint{1600.0f, 900.0f}, LayoutPoint{1000.0f, 1000.0f}, LayoutPoint{420.0f, 900.0f},
          LayoutPoint{2400.0f, 700.0f}, LayoutPoint{700.0f, 2400.0f}}) {
        CHECK_FALSE(overlaps(layoutFor(LayoutPoint{17.0f, 23.0f}, size)));
    }
}

TEST_CASE("Neither rectangle escapes the space it was given")
{
    const LayoutPoint origin{17.0f, 23.0f};
    const LayoutPoint size{1280.0f, 900.0f};
    const ShellLayout layout = layoutFor(origin, size);

    CHECK(layout.screenPos.x >= origin.x);
    CHECK(layout.screenPos.y >= origin.y);
    CHECK(layout.appPos.x + layout.appSize.x <= origin.x + size.x);
    CHECK(layout.appPos.y + layout.appSize.y <= origin.y + size.y);
}

TEST_CASE("A narrow viewport does not let the application eat the picture")
{
    // The window has a fixed width until there is not room for it, and then it
    // takes half - never more, or the thing being measured is smaller than the
    // instrument measuring it.
    const ShellLayout layout = layoutFor(LayoutPoint{0.0f, 0.0f}, LayoutPoint{500.0f, 400.0f});

    CHECK(layout.screenSize.x >= 250.0f);
}

TEST_CASE("The Lab starter region is a compact square centred in the screen workspace")
{
    for (const LayoutPoint size : {LayoutPoint{1400.0f, 800.0f}, LayoutPoint{600.0f, 1200.0f}}) {
        const ShellLayout layout = layoutFor(LayoutPoint{17.0f, 23.0f}, size);
        const LayoutRect region = starterRegionFor(layout);
        const float screenCentreX = layout.screenPos.x + layout.screenSize.x * 0.5f;
        const float screenCentreY = layout.screenPos.y + layout.screenSize.y * 0.5f;

        CHECK(region.size.x == region.size.y);
        CHECK(region.size.x == std::min(layout.screenSize.x, layout.screenSize.y) * StarterRegionShare);
        CHECK(region.position.x + region.size.x * 0.5f == screenCentreX);
        CHECK(region.position.y + region.size.y * 0.5f == screenCentreY);
        CHECK(region.position.x >= layout.screenPos.x);
        CHECK(region.position.y >= layout.screenPos.y);
        CHECK(region.position.x + region.size.x <= layout.screenPos.x + layout.screenSize.x);
        CHECK(region.position.y + region.size.y <= layout.screenPos.y + layout.screenSize.y);
    }
}

TEST_CASE("A virtual display capture includes black pixels outside the picture")
{
    // A 2x2 BGRA picture sits in the middle of a 4x4 selected display region.
    const std::vector<uint8_t> picture{
        1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255,
    };
    const LabDisplayCapture capture = captureVirtualDisplayRegion(
        LayoutRect{LayoutPoint{0.0f, 0.0f}, LayoutPoint{4.0f, 4.0f}},
        LayoutRect{LayoutPoint{1.0f, 1.0f}, LayoutPoint{2.0f, 2.0f}}, LayoutPoint{2.0f, 2.0f}, picture);

    REQUIRE(capture.width == 4);
    REQUIRE(capture.height == 4);
    REQUIRE(capture.bgra.size() == 64);
    CHECK(std::vector<uint8_t>(capture.bgra.begin(), capture.bgra.begin() + 4) == std::vector<uint8_t>{0, 0, 0, 255});
    const std::size_t pictureTopLeft = (static_cast<std::size_t>(1) * capture.width + 1) * 4u;
    CHECK(std::vector<uint8_t>(capture.bgra.begin() + static_cast<std::ptrdiff_t>(pictureTopLeft),
                               capture.bgra.begin() + static_cast<std::ptrdiff_t>(pictureTopLeft + 4)) ==
          std::vector<uint8_t>{1, 2, 3, 255});
    const std::size_t pictureBottomRight = (static_cast<std::size_t>(2) * capture.width + 2) * 4u;
    CHECK(std::vector<uint8_t>(capture.bgra.begin() + static_cast<std::ptrdiff_t>(pictureBottomRight),
                               capture.bgra.begin() + static_cast<std::ptrdiff_t>(pictureBottomRight + 4)) ==
          std::vector<uint8_t>{10, 11, 12, 255});
    CHECK(std::vector<uint8_t>(capture.bgra.end() - 4, capture.bgra.end()) == std::vector<uint8_t>{0, 0, 0, 255});
}

TEST_CASE("A virtual display capture retains the fitted picture's source density")
{
    const std::vector<uint8_t> picture{
        1,  2,  3,  255, 4,  5,  6,  255, 7,  8,  9,  255, 10, 11, 12, 255,
        13, 14, 15, 255, 16, 17, 18, 255, 19, 20, 21, 255, 22, 23, 24, 255,
    };
    const LabDisplayCapture capture = captureVirtualDisplayRegion(
        LayoutRect{LayoutPoint{10.0f, 20.0f}, LayoutPoint{2.0f, 1.0f}},
        LayoutRect{LayoutPoint{10.0f, 20.0f}, LayoutPoint{2.0f, 1.0f}}, LayoutPoint{4.0f, 2.0f}, picture);

    CHECK(capture.width == 4);
    CHECK(capture.height == 2);
    CHECK(capture.bgra == picture);
}

TEST_CASE("A virtual display capture bounds extreme source density")
{
    const std::vector<uint8_t> picture(static_cast<std::size_t>(100) * 100 * 4u, 127);
    const LabDisplayCapture capture = captureVirtualDisplayRegion(
        LayoutRect{LayoutPoint{0.0f, 0.0f}, LayoutPoint{2.0f, 3.0f}},
        LayoutRect{LayoutPoint{0.0f, 0.0f}, LayoutPoint{1.0f, 1.0f}}, LayoutPoint{100.0f, 100.0f}, picture);

    CHECK(capture.width == 8);
    CHECK(capture.height == 12);
}

}  // namespace sidescopes
