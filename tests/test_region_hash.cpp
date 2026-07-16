#include <catch2/catch_test_macros.hpp>

#include "core/region_hash.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

TEST_CASE("hashRegion is stable for identical content")
{
    TestFrame frame(64, 64);
    frame.setPixel(10, 8, 200);
    const IntRect region{0, 0, 64, 64};

    CHECK(hashRegion(frame.view(), region) == hashRegion(frame.view(), region));
}

TEST_CASE("hashRegion changes when content inside the region changes")
{
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 64, 64};
    const uint64_t before = hashRegion(frame.view(), region);

    frame.setPixel(10, 8, 200);  // row 8 is sampled (multiple of 4)
    CHECK(hashRegion(frame.view(), region) != before);
}

TEST_CASE("hashRegion ignores changes outside the region")
{
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 32, 32};
    const uint64_t before = hashRegion(frame.view(), region);

    frame.setPixel(50, 48, 200);
    CHECK(hashRegion(frame.view(), region) == before);
}

TEST_CASE("hashRegion ignores changes inside the masked area")
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

TEST_CASE("hashRegion of an empty region is stable")
{
    TestFrame frame(64, 64);
    CHECK(hashRegion(frame.view(), IntRect{200, 200, 10, 10}) == hashRegion(frame.view(), IntRect{300, 300, 10, 10}));
}

TEST_CASE("hashRegion ignores changes on unsampled rows")
{
    // The hash samples every fourth row, so a change on a row it steps over
    // is invisible; the very next sampled row is not. This pins the sampling
    // stride the skip logic trades accuracy for speed with.
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 64, 64};
    const uint64_t before = hashRegion(frame.view(), region);

    frame.setPixel(10, 9, 200);  // row 9 sits between sampled rows 8 and 12
    CHECK(hashRegion(frame.view(), region) == before);

    frame.setPixel(10, 8, 200);  // row 8 is sampled
    CHECK(hashRegion(frame.view(), region) != before);
}

TEST_CASE("hashRegion handles a mask that overhangs the region edge")
{
    // The mask extends past the region's right and bottom edges. Content it
    // covers inside the region is excluded; content left of it still votes,
    // and the out-of-region overhang neither crashes nor counts.
    TestFrame frame(32, 32);
    const IntRect region{0, 0, 32, 32};
    const IntRect masked{16, 16, 64, 64};  // overhangs both far edges
    const uint64_t before = hashRegion(frame.view(), region, masked);

    frame.setPixel(20, 16, 200);  // masked, on sampled row 16
    CHECK(hashRegion(frame.view(), region, masked) == before);

    frame.setPixel(4, 16, 200);  // left of the mask, same sampled row
    CHECK(hashRegion(frame.view(), region, masked) != before);
}

}  // namespace sidescopes
