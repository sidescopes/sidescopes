#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "app/region_border_motion.h"
#include "core/analysis_worker.h"

using namespace sidescopes;

namespace {

constexpr RegionOfInterest Start{10, 20, 30, 40};
constexpr RegionOfInterest Finish{50, 45, 80, 75};

std::array<double, 4> edges(const RegionOfInterest& value)
{
    return {value.leftPercent, value.topPercent, value.rightPercent, value.bottomPercent};
}

void nearRegion(const RegionOfInterest& actual, const RegionOfInterest& expected, double tolerance = 1e-10)
{
    const auto a = edges(actual);
    const auto b = edges(expected);
    for (std::size_t index = 0; index < a.size(); ++index) {
        CHECK(a[index] == Catch::Approx(b[index]).margin(tolerance));
    }
}

RegionOfInterest stepResponse(const RegionOfInterest& from, const RegionOfInterest& to, double seconds)
{
    const auto a = edges(from);
    const auto b = edges(to);
    std::array<double, 4> result{};
    // The unit critically damped step response from rest has this closed form.
    const double remaining = (1 + 40 * seconds) * std::exp(-40 * seconds);
    for (std::size_t index = 0; index < a.size(); ++index) {
        result[index] = b[index] + (a[index] - b[index]) * remaining;
    }
    return {result[0], result[1], result[2], result[3]};
}

}  // namespace

TEST_CASE("Border geometry starts at the target and can be discarded")
{
    RegionBorderMotion motion;
    CHECK_FALSE(motion.active());
    CHECK(motion.update(Start, 1.0, true) == Start);
    CHECK_FALSE(motion.active());
    CHECK(motion.update(Finish, 1.0, true) == Start);
    CHECK(motion.active());
    motion.reset();
    CHECK_FALSE(motion.active());
    CHECK(motion.update(Finish, 1.01, true) == Finish);
    CHECK_FALSE(motion.active());
}

TEST_CASE("A new border target is not integrated before it arrives")
{
    RegionBorderMotion motion;
    (void)motion.update(Start, 1.0, true);
    CHECK(motion.update(Finish, 1.20, true) == Start);
    CHECK(motion.update(Finish, 1.20, true) == Start);
    nearRegion(motion.update(Finish, 1.225, true), stepResponse(Start, Finish, 0.025));
    CHECK(motion.active());
}

TEST_CASE("A border step from rest approaches without stationary overshoot and settles exactly")
{
    RegionBorderMotion motion;
    (void)motion.update(Start, 1.0, true);
    (void)motion.update(Finish, 1.0, true);
    auto previous = edges(Start);
    for (int index = 1; index <= 120; ++index) {
        const auto current = edges(motion.update(Finish, 1.0 + index / 120.0, true));
        const auto target = edges(Finish);
        for (std::size_t edge = 0; edge < current.size(); ++edge) {
            CHECK(current[edge] >= previous[edge]);
            CHECK(current[edge] <= target[edge]);
        }
        previous = current;
    }
    CHECK_FALSE(motion.active());
    CHECK(motion.update(Finish, 2.01, true) == Finish);
}

TEST_CASE("Border spring evolution does not depend on regular or irregular presentation cadence")
{
    RegionBorderMotion single;
    RegionBorderMotion regular;
    RegionBorderMotion irregular;
    for (auto* motion : {&single, &regular, &irregular}) {
        (void)motion->update(Start, 1.0, true);
        (void)motion->update(Finish, 1.0, true);
    }
    const auto expected = single.update(Finish, 1.15, true);
    RegionOfInterest actual;
    for (int index = 1; index <= 15; ++index) {
        actual = regular.update(Finish, 1.0 + index / 100.0, true);
    }
    nearRegion(actual, expected);
    for (const double offset : {0.003, 0.029, 0.031, 0.078, 0.081, 0.132, 0.15}) {
        actual = irregular.update(Finish, 1.0 + offset, true);
    }
    nearRegion(actual, expected);
    nearRegion(expected, stepResponse(Start, Finish, 0.15));
}

TEST_CASE("Border retargeting retains position and velocity at a reversal")
{
    RegionBorderMotion continued;
    RegionBorderMotion reversed;
    for (auto* motion : {&continued, &reversed}) {
        (void)motion->update(Start, 1.0, true);
        (void)motion->update(Finish, 1.0, true);
    }
    const auto position = continued.update(Finish, 1.03, true);
    nearRegion(reversed.update(Start, 1.03, true), position);
    constexpr double Step = 1e-6;
    const auto nextContinued = continued.update(Finish, 1.03 + Step, true);
    const auto nextReversed = reversed.update(Start, 1.03 + Step, true);
    // A different target changes acceleration immediately, not the existing velocity.
    CHECK(nextReversed.leftPercent > position.leftPercent);
    const double continuingSpeed = (nextContinued.leftPercent - position.leftPercent) / Step;
    const double reversingSpeed = (nextReversed.leftPercent - position.leftPercent) / Step;
    CHECK(std::abs(continuingSpeed - reversingSpeed) < 0.04);
}

TEST_CASE("Positive border widths and aspect survive repeated pan zoom and reversals")
{
    RegionBorderMotion motion;
    (void)motion.update(Start, 1.0, true);
    for (int index = 1; index <= 600; ++index) {
        const double side = index % 3 == 0 ? 0.01 : index % 3 == 1 ? 70.0 : 5.0;
        const double left = index % 2 == 0 ? 5.0 : 25.0;
        const RegionOfInterest target{left, 10.0, left + side, 10.0 + side};
        const auto current = motion.update(target, 1.0 + index / 120.0, true);
        CHECK(current.leftPercent < current.rightPercent);
        CHECK(current.topPercent < current.bottomPercent);
        CHECK(current.rightPercent - current.leftPercent ==
              Catch::Approx(current.bottomPercent - current.topPercent).margin(1e-10));
        CHECK(current.leftPercent >= 0.0);
        CHECK(current.topPercent >= 0.0);
        CHECK(current.rightPercent <= 100.0);
        CHECK(current.bottomPercent <= 100.0);
    }
}

TEST_CASE("Manual border placement stale clocks and long gaps cancel animation")
{
    RegionBorderMotion motion;
    (void)motion.update(Start, 1.0, true);
    (void)motion.update(Finish, 1.01, true);
    REQUIRE(motion.active());
    double snapTime = 1.02;
    SECTION("manual placement")
    {
        CHECK(motion.update(Finish, 1.02, false) == Finish);
    }
    SECTION("backwards clock")
    {
        snapTime = 0.9;
        CHECK(motion.update(Finish, snapTime, true) == Finish);
    }
    SECTION("stalled presentation")
    {
        snapTime = 1.261;
        CHECK(motion.update(Finish, snapTime, true) == Finish);
    }
    CHECK_FALSE(motion.active());
    CHECK(motion.update(Start, snapTime + 0.01, true) == Finish);
    CHECK(motion.active());
}

TEST_CASE("Border settling requires both small edge error and small velocity")
{
    RegionBorderMotion motion;
    (void)motion.update(Start, 1.0, true);
    SECTION("error threshold is in percentage points")
    {
        const RegionOfInterest tiny{10.0019, 20, 30.0019, 40};
        CHECK(motion.update(tiny, 1.0, true) == tiny);
        CHECK_FALSE(motion.active());
        const RegionOfInterest larger{10.004, 20, 30.004, 40};
        CHECK(motion.update(larger, 1.0, true) == tiny);
        CHECK(motion.active());
    }
    SECTION("matching position does not erase nonzero velocity")
    {
        (void)motion.update(Finish, 1.0, true);
        const auto position = motion.update(Finish, 1.03, true);
        CHECK(motion.update(position, 1.03, true) == position);
        CHECK(motion.active());
        CHECK(motion.update(position, 1.031, true).leftPercent > position.leftPercent);
    }
}
