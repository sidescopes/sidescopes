#include <catch2/catch_test_macros.hpp>

#include "core/region_hash.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

TEST_CASE("HashRegion is stable for identical content")
{
    TestFrame frame(64, 64);
    frame.setPixel(10, 8, 200);
    const IntRect region{0, 0, 64, 64};

    CHECK(hashRegion(frame.view(), region) == hashRegion(frame.view(), region));
}

TEST_CASE("HashRegion changes when content inside the region changes")
{
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 64, 64};
    const uint64_t before = hashRegion(frame.view(), region);

    frame.setPixel(10, 8, 200);  // row 8 is sampled (multiple of 4)
    CHECK(hashRegion(frame.view(), region) != before);
}

TEST_CASE("HashRegion ignores changes outside the region")
{
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 32, 32};
    const uint64_t before = hashRegion(frame.view(), region);

    frame.setPixel(50, 48, 200);
    CHECK(hashRegion(frame.view(), region) == before);
}

TEST_CASE("HashRegion ignores changes inside the masked area")
{
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 64, 64};
    const IntRect masked{16, 16, 32, 32};
    const uint64_t before = hashRegion(frame.view(), region, masked);

    frame.setPixel(31, 24, 200);  // inside the mask
    CHECK(hashRegion(frame.view(), region, masked) == before);

    frame.setPixel(2, 24, 77);  // same row, left of the mask
    CHECK(hashRegion(frame.view(), region, masked) != before);
}

TEST_CASE("HashRegion of an empty region is stable")
{
    TestFrame frame(64, 64);
    CHECK(hashRegion(frame.view(), IntRect{200, 200, 10, 10}) == hashRegion(frame.view(), IntRect{300, 300, 10, 10}));
}

}  // namespace sidescopes
