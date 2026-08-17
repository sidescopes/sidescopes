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

TEST_CASE("A global region maps only its overlap with the supplied picture")
{
    const LayoutRect picture{LayoutPoint{100.0f, 50.0f}, LayoutPoint{400.0f, 300.0f}};
    const LayoutPoint pixels{800.0f, 600.0f};

    const auto inside =
        picturePixelsUnderRegion(LayoutRect{LayoutPoint{200.0f, 100.0f}, LayoutPoint{100.0f, 50.0f}}, picture, pixels);
    REQUIRE(inside);
    CHECK(inside->position.x == 200.0f);
    CHECK(inside->position.y == 100.0f);
    CHECK(inside->size.x == 200.0f);
    CHECK(inside->size.y == 100.0f);

    const auto crossing =
        picturePixelsUnderRegion(LayoutRect{LayoutPoint{50.0f, 0.0f}, LayoutPoint{100.0f, 100.0f}}, picture, pixels);
    REQUIRE(crossing);
    CHECK(crossing->position.x == 0.0f);
    CHECK(crossing->position.y == 0.0f);
    CHECK(crossing->size.x == 100.0f);
    CHECK(crossing->size.y == 100.0f);

    CHECK_FALSE(
        picturePixelsUnderRegion(LayoutRect{LayoutPoint{520.0f, 100.0f}, LayoutPoint{80.0f, 80.0f}}, picture, pixels));
}

}  // namespace sidescopes
