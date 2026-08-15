#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "web/band_geometry.h"

namespace sidescopes {
namespace {

/// The band as the region editor builds it: the region grown by the ring for
/// the hole, and by the pad for the outer edge.
struct Band
{
    BandRect outer;
    BandRect hole;
};

[[nodiscard]] Band bandAround(float left, float top, float right, float bottom, float pad = 12.0f, float ring = 1.0f)
{
    return Band{BandRect{left - pad, top - pad, right + pad, bottom + pad},
                BandRect{left - ring, top - ring, right + ring, bottom + ring}};
}

[[nodiscard]] bool whole(float value)
{
    return value == std::floor(value);
}

[[nodiscard]] float overlapArea(const BandRect& one, const BandRect& other)
{
    const BandRect shared{std::max(one.left, other.left), std::max(one.top, other.top),
                          std::min(one.right, other.right), std::min(one.bottom, other.bottom)};

    return shared.area();
}

}  // namespace

TEST_CASE("Adjacent quarters share the very same edge")
{
    // THE seam. Each quarter draws the whole hazard ruling, so a row claimed
    // by two of them takes the stripe ink twice and reads as a light line
    // ruled across the band. It shipped that way twice.
    const Band band = bandAround(40.3f, 60.7f, 300.2f, 220.9f);
    const auto quarter = bandQuarters(band.outer, band.hole);

    CHECK(quarter[0].bottom == quarter[2].top);  // top meets left
    CHECK(quarter[0].bottom == quarter[3].top);  // top meets right
    CHECK(quarter[1].top == quarter[2].bottom);  // bottom meets left
    CHECK(quarter[1].top == quarter[3].bottom);  // bottom meets right
    CHECK(quarter[2].right <= quarter[3].left);  // the sides do not meet at all
}

TEST_CASE("Every edge is a whole point, whatever the region lands on")
{
    // A clip becomes a scissor box, and a scissor box is integers. A
    // fractional edge is the one that gets truncated into both neighbours.
    for (const float nudge : {0.0f, 0.1f, 0.49f, 0.5f, 0.51f, 0.99f}) {
        const Band band = bandAround(40.0f + nudge, 60.0f + nudge, 300.0f + nudge, 220.0f + nudge);
        for (const BandRect& rect : bandQuarters(band.outer, band.hole)) {
            CHECK(whole(rect.left));
            CHECK(whole(rect.top));
            CHECK(whole(rect.right));
            CHECK(whole(rect.bottom));
        }
    }
}

TEST_CASE("No two quarters overlap")
{
    const Band band = bandAround(11.6f, 23.4f, 190.1f, 88.8f);
    const auto quarter = bandQuarters(band.outer, band.hole);

    for (std::size_t one = 0; one < quarter.size(); ++one) {
        for (std::size_t other = one + 1; other < quarter.size(); ++other) {
            CHECK(overlapArea(quarter[one], quarter[other]) == 0.0f);
        }
    }
}

TEST_CASE("The quarters cover the ring exactly, with nothing left over")
{
    // A gap here is not invisible either: it shows as a hairline of the
    // photograph between the band's stripes and the region's own dashed edge.
    const Band band = bandAround(40.3f, 60.7f, 300.2f, 220.9f);
    const auto quarter = bandQuarters(band.outer, band.hole);

    const BandRect outer{std::round(band.outer.left), std::round(band.outer.top), std::round(band.outer.right),
                         std::round(band.outer.bottom)};
    const BandRect hole{std::round(band.hole.left), std::round(band.hole.top), std::round(band.hole.right),
                        std::round(band.hole.bottom)};
    float covered = 0.0f;
    for (const BandRect& rect : quarter) {
        covered += rect.area();
    }

    CHECK(covered == outer.area() - hole.area());
}

TEST_CASE("The region's interior is never covered")
{
    // The band is drawn OVER the picture and the interior is what the scopes
    // measure; a quarter reaching into it would tint the very pixels being
    // read, which is the one thing this border must not do.
    const Band band = bandAround(40.0f, 60.0f, 300.0f, 220.0f);

    for (const BandRect& rect : bandQuarters(band.outer, band.hole)) {
        CHECK(overlapArea(rect, band.hole) == 0.0f);
    }
}

TEST_CASE("A hole bigger than the band leaves empty quarters, not inverted ones")
{
    // Real, not hypothetical: the band is clipped to the picture, so a region
    // pushed hard against an edge has less room outside it than the band wants.
    const BandRect outer{100.0f, 100.0f, 200.0f, 200.0f};
    const BandRect hole{50.0f, 50.0f, 260.0f, 260.0f};

    for (const BandRect& rect : bandQuarters(outer, hole)) {
        CHECK(rect.right >= rect.left);
        CHECK(rect.bottom >= rect.top);
        CHECK(rect.area() == 0.0f);
    }
}

TEST_CASE("A region already on whole points is left where it is")
{
    // The editor snaps the region before asking for quarters, so the usual
    // case must be a no-op rather than a half-point shift of the whole border.
    const Band band = bandAround(40.0f, 60.0f, 300.0f, 220.0f);
    const auto quarter = bandQuarters(band.outer, band.hole);

    CHECK(quarter[0].top == 48.0f);
    CHECK(quarter[0].bottom == 59.0f);
    CHECK(quarter[2].left == 28.0f);
    CHECK(quarter[2].right == 39.0f);
}

}  // namespace sidescopes
