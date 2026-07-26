#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "core/marker_smoother.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

TEST_CASE("averageNeighborhood averages the full window")
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

TEST_CASE("averageNeighborhood clips at frame edges")
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

namespace {

// Frames at sixty a second until the smoother has locked onto @p target.
int framesToArrive(MarkerSmoother& smoother, const FloatColor& target)
{
    for (int frame = 1; frame <= 600; ++frame) {
        const FloatColor value = smoother.update(target, 1.0f / 60.0f);
        if (value.r == target.r && value.g == target.g && value.b == target.b) {
            return frame;
        }
    }

    return -1;
}

}  // namespace

TEST_CASE("MarkerSmoother arrives in about the same time whatever the distance")
{
    // The regression this guards is a usability one: a fixed time constant
    // needs a further one for every e-fold of distance, so a marker sent right
    // across the range took half again as long as one sent a tenth of the way,
    // and the reading a pointer was moved to arrive at kept the pointer
    // waiting. Dividing the constant by the distance bounds that.
    MarkerSmoother near;
    near.setTimeConstant(100.0f);
    const int nearFrames = framesToArrive(near, FloatColor{136.0f, 128.0f, 128.0f});

    MarkerSmoother far;
    far.setTimeConstant(100.0f);
    const int farFrames = framesToArrive(far, FloatColor{255.0f, 128.0f, 128.0f});

    REQUIRE(nearFrames > 0);
    REQUIRE(farFrames > 0);
    // Thirty-one times the distance, and at most four more frames to cross it.
    CHECK(farFrames - nearFrames <= 4);
    // A fifth of a second at sixty frames a second, whatever the distance.
    CHECK(farFrames <= 13);
}

TEST_CASE("MarkerSmoother still eases movement close to its target")
{
    // The other half of the same knob. Speeding up with distance must not turn
    // into snapping: a sample that moves a reading by a code is jitter, and
    // absorbing it is what the smoothing is for. One frame of a 100 ms constant
    // covers 1 - e^(-1/6) of the way, and a code out is close enough to its
    // target that the distance term may barely touch that.
    MarkerSmoother smoother;
    smoother.setTimeConstant(100.0f);
    const FloatColor target{129.0f, 128.0f, 128.0f};

    const FloatColor stepped = smoother.update(target, 1.0f / 60.0f);
    const float covered = stepped.r - 128.0f;
    CHECK(covered > 0.14f);
    CHECK(covered < 0.20f);
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
