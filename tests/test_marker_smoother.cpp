#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "core/marker_smoother.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

TEST_CASE("AverageNeighborhood averages the full window")
{
    TestFrame frame(3, 3);
    for (int py = 0; py < 3; ++py) {
        for (int px = 0; px < 3; ++px) {
            frame.setColor(px, py, Color{90, 0, 0});
        }
    }
    frame.setColor(1, 1, Color{180, 0, 0});

    const FloatColor average = averageNeighborhood(frame.view(), 1, 1);
    CHECK(average.r > 99.0f);  // (8 * 90 + 180) / 9 = 100
    CHECK(average.r < 101.0f);
}

TEST_CASE("AverageNeighborhood clips at frame edges")
{
    TestFrame frame(2, 2);
    frame.setColor(0, 0, Color{100, 100, 100});
    frame.setColor(1, 0, Color{100, 100, 100});
    frame.setColor(0, 1, Color{100, 100, 100});
    frame.setColor(1, 1, Color{100, 100, 100});

    const FloatColor average = averageNeighborhood(frame.view(), 0, 0);
    CHECK(average.r == 100.0f);  // four valid samples, all identical
}

TEST_CASE("MarkerSmoother converges monotonically and snaps")
{
    MarkerSmoother smoother;
    smoother.setTimeConstant(100.0f);
    const FloatColor target{200.0f, 60.0f, 20.0f};

    float previousDistance = 1e9f;
    bool snapped = false;
    for (int step = 0; step < 200; ++step) {
        const FloatColor value = smoother.update(target, 1.0f / 60.0f);
        const float distance =
            std::abs(target.r - value.r) + std::abs(target.g - value.g) + std::abs(target.b - value.b);
        REQUIRE(distance <= previousDistance);
        previousDistance = distance;
        if (distance == 0.0f) {
            snapped = true;
            break;
        }
    }
    // The exponential tail alone never reaches zero; the snap window must
    // end it in finite time. This is the regression test for the marker
    // dithering between adjacent scope bins while settling.
    CHECK(snapped);
}

TEST_CASE("MarkerSmoother stays locked once on target")
{
    MarkerSmoother smoother;
    smoother.setTimeConstant(100.0f);
    const FloatColor target{50.0f, 51.0f, 52.0f};

    for (int step = 0; step < 300; ++step) {
        smoother.update(target, 1.0f / 60.0f);
    }
    const FloatColor settled = smoother.update(target, 1.0f / 60.0f);
    CHECK(settled.r == target.r);
    CHECK(settled.g == target.g);
    CHECK(settled.b == target.b);
}

TEST_CASE("MarkerSmoother with a zero time constant follows immediately")
{
    MarkerSmoother smoother;
    smoother.setTimeConstant(0.0f);
    const FloatColor value = smoother.update(FloatColor{10.0f, 20.0f, 30.0f}, 1.0f / 120.0f);
    CHECK(value.r == 10.0f);
    CHECK(value.g == 20.0f);
    CHECK(value.b == 30.0f);
}

}  // namespace sidescopes
