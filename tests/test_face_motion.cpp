#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "app/face_motion.h"

using namespace sidescopes;

namespace {

constexpr FaceAnchor Initial{500.0, 500.0, 200.0};

FaceLockState selection()
{
    return face_lock::makeLock(Initial, {450.0, 380.0, 550.0, 440.0});
}

std::array<double, 4> edges(const FaceLockState& state, FaceAnchor anchor)
{
    const auto crop = face_lock::mapRegion(state, anchor);
    return {crop.left, crop.top, crop.right, crop.bottom};
}

void sameAnchor(FaceAnchor actual, FaceAnchor expected)
{
    CHECK(actual.centerX == Catch::Approx(expected.centerX));
    CHECK(actual.centerY == Catch::Approx(expected.centerY));
    CHECK(actual.width == Catch::Approx(expected.width));
}

}  // namespace

TEST_CASE("Face motion starts and resumes from current evidence without a stale glide")
{
    FaceMotion motion;
    motion.reset(selection());
    const FaceAnchor first{505, 490, 205};
    sameAnchor(motion.follow(first, 10.0), first);
    (void)motion.follow({506, 490, 205}, 10.05);
    SECTION("unchanged capture interval")
    {
        const FaceAnchor next{510, 490, 205};
        sameAnchor(motion.follow(next, 11.0), next);
    }
    SECTION("uncertainty")
    {
        const auto held = motion.current();
        motion.pause();
        sameAnchor(motion.current(), held);
        const FaceAnchor next{510, 490, 205};
        sameAnchor(motion.follow(next, 10.10), next);
    }
    SECTION("manual crop and window remapping")
    {
        const FaceAnchor moved{600, 600, 150};
        motion.reset(face_lock::makeLock(moved, {550, 450, 650, 475}));
        sameAnchor(motion.current(), moved);
        sameAnchor(motion.follow(moved, 10.10), moved);
    }
}

TEST_CASE("A fixed detected face never acquires artificial motion")
{
    FaceMotion motion;
    motion.reset(selection());
    for (int index = 0; index < 120; ++index) {
        const auto value = motion.follow(Initial, 10.0 + index / 30.0);
        CHECK(value.centerX == Initial.centerX);
        CHECK(value.centerY == Initial.centerY);
        CHECK(value.width == Initial.width);
    }
}

TEST_CASE("Small alternating face box errors lose at least half their edge shake")
{
    for (const int rate : {15, 20, 30, 60}) {
        CAPTURE(rate);
        FaceMotion motion;
        const auto state = selection();
        motion.reset(state);
        (void)motion.follow(Initial, 0.0);
        auto previousRaw = edges(state, Initial);
        auto previousSmooth = previousRaw;
        double rawTravel = 0.0;
        double smoothTravel = 0.0;
        for (int index = 1; index <= rate * 3; ++index) {
            const double sign = index % 2 == 0 ? 1.0 : -1.0;
            const FaceAnchor target{500.0 + sign * 2.0, 500.0 - sign, 200.0 + sign * 2.0};
            const auto raw = edges(state, target);
            const auto smooth = edges(state, motion.follow(target, double(index) / rate));
            for (std::size_t edge = 0; edge < raw.size(); ++edge) {
                rawTravel += std::abs(raw[edge] - previousRaw[edge]);
                smoothTravel += std::abs(smooth[edge] - previousSmooth[edge]);
            }
            previousRaw = raw;
            previousSmooth = smooth;
        }
        CHECK(smoothTravel < rawTravel * 0.5);
    }
}

TEST_CASE("Fast photo panning has bounded added lag without overshooting a reversal")
{
    for (const int rate : {15, 20, 30, 60}) {
        CAPTURE(rate);
        FaceMotion motion;
        motion.reset(selection());
        auto previous = motion.follow(Initial, 0.0);
        for (int index = 1; index <= rate * 2; ++index) {
            const double time = double(index) / rate;
            const double displacement = 200.0 * (time <= 1.0 ? time : 2.0 - time);
            const FaceAnchor target{500.0 + displacement, 500.0, 200.0};
            const auto current = motion.follow(target, time);
            // At one face width per second, a 4-pixel residual is 20 ms.
            CHECK(std::abs(current.centerX - target.centerX) <= 4.0 + 1e-9);
            CHECK(current.centerX >= std::min(previous.centerX, target.centerX) - 1e-9);
            CHECK(current.centerX <= std::max(previous.centerX, target.centerX) + 1e-9);
            previous = current;
        }
    }
}

TEST_CASE("Combined pan and zoom keep every crop edge between two valid crops")
{
    FaceMotion motion;
    const auto state = selection();
    motion.reset(state);
    (void)motion.follow(Initial, 10.0);
    auto previous = edges(state, Initial);
    for (int index = 1; index <= 120; ++index) {
        const double phase = index * 0.1;
        const FaceAnchor target{500.0 + 50.0 * std::sin(phase), 500.0 + 30.0 * std::cos(phase),
                                200.0 + 40.0 * std::sin(phase * 0.7)};
        const auto raw = edges(state, target);
        const auto current = edges(state, motion.follow(target, 10.0 + index / 60.0));
        for (std::size_t edge = 0; edge < raw.size(); ++edge) {
            CHECK(current[edge] >= std::min(previous[edge], raw[edge]) - 1e-9);
            CHECK(current[edge] <= std::max(previous[edge], raw[edge]) + 1e-9);
            CHECK(std::abs(current[edge] - raw[edge]) <= 0.02 * target.width + 1e-9);
        }
        CHECK((current[2] - current[0]) / (current[3] - current[1]) == Catch::Approx(100.0 / 60.0));
        previous = current;
    }
}

TEST_CASE("A stopped photo retains a bounded final crop without inventing fresh frames")
{
    FaceMotion motion;
    // A large offset magnifies width noise: bound actual crop edges, not
    // merely the face center or width considered in isolation.
    const auto state = face_lock::makeLock(Initial, {600, 100, 900, 150});
    motion.reset(state);
    (void)motion.follow(Initial, 10.0);
    const FaceAnchor target{504, 502, 202};
    const auto final = motion.follow(target, 10.01);
    const auto actual = edges(state, final);
    const auto wanted = edges(state, target);
    for (std::size_t edge = 0; edge < actual.size(); ++edge) {
        CHECK(std::abs(actual[edge] - wanted[edge]) <= 0.02 * target.width + 1e-9);
    }
    sameAnchor(motion.current(), final);
}

TEST_CASE("Face smoothing scales with the face and ignores desktop translation")
{
    FaceMotion reference;
    reference.reset(selection());
    FaceMotion transformed;
    constexpr double Scale = 2.75;
    const auto transform = [](FaceAnchor value) {
        return FaceAnchor{value.centerX * Scale + 1000, value.centerY * Scale + 200, value.width * Scale};
    };
    auto state = selection();
    state.lastAnchor = transform(Initial);
    transformed.reset(state);
    for (int index = 0; index < 100; ++index) {
        const FaceAnchor target{500.0 + std::sin(index * 0.2) * 10, 500.0, 200.0 + std::cos(index * 0.3) * 2};
        const auto expected = transform(reference.follow(target, index / 20.0));
        sameAnchor(transformed.follow(transform(target), index / 20.0), expected);
    }
}

TEST_CASE("Repeated or older times cannot advance face motion")
{
    FaceMotion motion;
    motion.reset(selection());
    (void)motion.follow(Initial, 10.0);
    const FaceAnchor next{520, 500, 200};
    sameAnchor(motion.follow(next, 10.0), Initial);
    sameAnchor(motion.follow(next, 9.9), Initial);
    CHECK(motion.follow(next, 10.05).centerX > Initial.centerX);
}
