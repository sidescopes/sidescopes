// The rectangle a software-narrowing capture backend copies for a requested
// crop: the request clamped to the display, and the whole display whenever
// the request cannot be honoured.

#include <catch2/catch_test_macros.hpp>
#include <optional>

#include "platform/software_crop.h"

using namespace sidescopes;

TEST_CASE("No crop copies the whole display")
{
    CHECK(softwareCropRect(std::nullopt, 3840, 2160) == IntRect{0, 0, 3840, 2160});
}

TEST_CASE("A crop inside the display is copied as asked")
{
    CHECK(softwareCropRect(IntRect{1553, 713, 734, 734}, 3840, 2160) == IntRect{1553, 713, 734, 734});
}

TEST_CASE("A crop overhanging the display edge is clamped to it")
{
    CHECK(softwareCropRect(IntRect{3600, 2000, 500, 500}, 3840, 2160) == IntRect{3600, 2000, 240, 160});
    CHECK(softwareCropRect(IntRect{-100, -50, 300, 300}, 3840, 2160) == IntRect{0, 0, 200, 250});
}

TEST_CASE("A crop with nothing inside the display falls back to the whole display")
{
    CHECK(softwareCropRect(IntRect{4000, 100, 200, 200}, 3840, 2160) == IntRect{0, 0, 3840, 2160});
    CHECK(softwareCropRect(IntRect{100, 100, 0, 200}, 3840, 2160) == IntRect{0, 0, 3840, 2160});
    CHECK(softwareCropRect(IntRect{100, 100, 200, -5}, 3840, 2160) == IntRect{0, 0, 3840, 2160});
}
