#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <stdexcept>
#include <vector>

#include "app/face_tracking.h"

using namespace sidescopes;
using namespace sidescopes::face_tracking;
using Catch::Matchers::WithinAbs;

namespace {

constexpr Context Source{1, 9, 3};
constexpr LockRect Bounds{0.0, 0.0, 4096.0, 2160.0};
constexpr LockRect Crop{450.0, 380.0, 550.0, 440.0};

LockRect face(double x, double y = 500.0, double width = 200.0)
{
    return {x - width / 2.0, y - width / 2.0, x + width / 2.0, y + width / 2.0};
}

Association selected()
{
    return {{Source, 1, 10.0}, face(500.0), Crop, Bounds, 10.0};
}

Decision observe(Association& policy, uint64_t sequence, double time, std::vector<LockRect> boxes)
{
    return policy.advance({Source, sequence, time}, time, DetectionStatus::Completed, boxes);
}

void checkCrop(const Decision& decision, LockRect expected)
{
    CHECK_THAT(decision.crop.left, WithinAbs(expected.left, 1e-8));
    CHECK_THAT(decision.crop.top, WithinAbs(expected.top, 1e-8));
    CHECK_THAT(decision.crop.right, WithinAbs(expected.right, 1e-8));
    CHECK_THAT(decision.crop.bottom, WithinAbs(expected.bottom, 1e-8));
}

}  // namespace

TEST_CASE("Continuous face motion immediately carries the selected crop")
{
    auto policy = selected();
    for (uint64_t index = 1; index <= 40; ++index) {
        const double movement = 50.0 * static_cast<double>(index);
        const auto result =
            observe(policy, index + 1, 10.0 + 0.05 * static_cast<double>(index), {face(500.0 + movement)});
        REQUIRE(result.action == Action::Accepted);
        checkCrop(result, {450.0 + movement, 380.0, 550.0 + movement, 440.0});
    }
}

TEST_CASE("A stopping or reversing face does not wait for matching positions")
{
    auto policy = selected();
    const std::array positions{540.0, 580.0, 580.0, 540.0, 500.0, 498.0, 502.0};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        const auto result =
            observe(policy, index + 2, 10.05 + 0.05 * static_cast<double>(index), {face(positions[index])});
        REQUIRE(result.action == Action::Accepted);
        CHECK(result.anchor.centerX == positions[index]);
    }
}

TEST_CASE("Unchanged photos do not lose tracking while capture withholds frames")
{
    auto policy = selected();
    CHECK(policy.tick(Source, 60.0).following);
    const auto resumed = observe(policy, 2, 60.01, {face(510.0)});
    REQUIRE(resumed.action == Action::Accepted);
    checkCrop(resumed, {460.0, 380.0, 560.0, 440.0});

    auto other = selected();
    // A long quiet interval does not authorize jumping to a distant face.
    const auto unrelated = observe(other, 2, 60.0, {face(1800.0)});
    CHECK(unrelated.action == Action::Held);
    checkCrop(unrelated, Crop);
}

TEST_CASE("A brief failed detection holds the crop and can resume following")
{
    auto policy = selected();
    const auto missing = observe(policy, 2, 10.05, {});
    CHECK(missing.action == Action::Held);
    checkCrop(missing, Crop);
    const auto resumed = observe(policy, 3, 10.10, {face(520.0)});
    CHECK(resumed.action == Action::Accepted);
    checkCrop(resumed, {470.0, 380.0, 570.0, 440.0});
}

TEST_CASE("A lost face becomes an ordinary crop and is never automatically selected again")
{
    auto policy = selected();
    REQUIRE(observe(policy, 2, 10.05, {face(520.0)}).action == Action::Accepted);
    REQUIRE(observe(policy, 3, 20.0, {}).action == Action::Held);
    const auto lost = policy.tick(Source, 20.41);
    CHECK(lost.action == Action::OrdinaryAttached);
    CHECK_FALSE(lost.following);
    checkCrop(lost, {470.0, 380.0, 570.0, 440.0});
    const auto returned = observe(policy, 4, 20.45, {face(540.0)});
    CHECK(returned.action == Action::OrdinaryAttached);
    checkCrop(returned, lost.crop);
}

TEST_CASE("A crossing stays uncertain when only one rival is visible again")
{
    auto policy = selected();
    const auto crossing = observe(policy, 2, 10.05, {face(490.0), face(520.0)});
    REQUIRE(crossing.reason == Reason::Ambiguous);
    const auto oneLeft = observe(policy, 3, 10.10, {face(520.0)});
    CHECK(oneLeft.action == Action::Held);
    CHECK(oneLeft.reason == Reason::Ambiguous);
    checkCrop(oneLeft, Crop);
    CHECK(policy.tick(Source, 10.46).action == Action::OrdinaryAttached);
}

TEST_CASE("Late or foreign detections cannot replace a newer selection")
{
    auto policy = selected();
    const std::array foreign{Context{2, 9, 3}, Context{1, 10, 3}, Context{1, 9, 4}};
    const std::array boxes{face(540.0)};
    for (const auto context : foreign) {
        const auto ignored = policy.advance({context, 2, 10.05}, 10.05, DetectionStatus::Completed, boxes);
        CHECK(ignored.action == Action::Ignored);
        CHECK(ignored.reason == Reason::StaleContext);
        checkCrop(ignored, Crop);
    }
    const auto accepted = observe(policy, 2, 10.05, {face(520.0)});
    REQUIRE(accepted.action == Action::Accepted);
    CHECK(observe(policy, 2, 10.10, {face(600.0)}).reason == Reason::DuplicateOrOlderFrame);
    const auto stale = policy.advance({Source, 3, 10.1}, 10.35, DetectionStatus::Completed, boxes);
    CHECK(stale.reason == Reason::ExpiredResult);
    checkCrop(stale, accepted.crop);
}

TEST_CASE("A manual crop changes shape without accepting old detection work")
{
    auto policy = selected();
    const LockRect edited{470.0, 400.0, 530.0, 430.0};
    const Context next{2, 9, 3};
    checkCrop(policy.rebindCrop(next, edited), edited);
    CHECK(observe(policy, 2, 10.05, {face(540.0)}).action == Action::Ignored);
    const std::array boxes{face(520.0, 500.0, 220.0)};
    const auto result = policy.advance({next, 2, 10.05}, 10.05, DetectionStatus::Completed, boxes);
    REQUIRE(result.action == Action::Accepted);
    checkCrop(result, {487.0, 390.0, 553.0, 423.0});
}

TEST_CASE("Native failures and invalid boxes cannot become face positions")
{
    auto policy = selected();
    const auto failed = policy.advance({Source, 2, 10.05}, 10.05, DetectionStatus::Failed, {});
    CHECK(failed.reason == Reason::NativeFailure);
    checkCrop(failed, Crop);
    const auto invalid = observe(policy, 3, 10.1, {{0.0, 0.0, std::numeric_limits<double>::infinity(), 100.0}});
    CHECK(invalid.reason == Reason::InvalidDetection);
    checkCrop(invalid, Crop);
    CHECK(policy.tick(Source, 10.46).action == Action::OrdinaryAttached);
}

TEST_CASE("Control remapping preserves uncertain and retired association state")
{
    auto policy = selected();
    REQUIRE(observe(policy, 2, 10.05, {face(490.0), face(520.0)}).reason == Reason::Ambiguous);
    const Context moved{2, Source.epoch, Source.display};
    const auto shifted = face_lock::makeLock({510, 500, 200}, {460, 380, 560, 440});
    CHECK(policy.remap(moved, shifted, Bounds).action == Action::Held);
    CHECK(policy.current().anchor.centerX == 510);
    CHECK(policy.tick(moved, 10.10).reason == Reason::Ambiguous);
    CHECK(policy.tick(moved, 10.46).action == Action::OrdinaryAttached);
    const Context edited{3, Source.epoch, Source.display};
    CHECK_FALSE(policy.remap(edited, shifted, Bounds).following);
    const std::array boxes{face(520.0)};
    CHECK(policy.advance({edited, 3, 10.50}, 10.50, DetectionStatus::Completed, boxes).action ==
          Action::OrdinaryAttached);
}

TEST_CASE("A control snapshot keeps its normalized crop despite a newer worker anchor")
{
    auto policy = selected();
    REQUIRE(observe(policy, 2, 10.05, {face(520.0, 505.0, 180.0)}).action == Action::Accepted);
    const Context next{2, Source.epoch, Source.display};
    const LockRect edited{470, 400, 530, 430};
    const auto saved = face_lock::makeLock({500, 500, 200}, edited);
    const auto remapped = policy.remap(next, saved, Bounds);
    REQUIRE(remapped.action == Action::Held);
    checkCrop(remapped, edited);
    CHECK(remapped.anchor.centerX == saved.lastAnchor.centerX);
    CHECK(remapped.anchor.centerY == saved.lastAnchor.centerY);
    CHECK(remapped.anchor.width == saved.lastAnchor.width);
    CHECK(remapped.evidenceSourceSeconds == 10.05);
    CHECK(observe(policy, 3, 10.10, {face(520.0)}).reason == Reason::StaleContext);
    const std::array boxes{face(520.0, 500.0, 220.0)};
    const auto followed = policy.advance({next, 3, 10.10}, 10.10, DetectionStatus::Completed, boxes);
    REQUIRE(followed.action == Action::Accepted);
    // The edited 0.3-by-0.15 crop follows the new raw face immediately.
    checkCrop(followed, {487, 390, 553, 423});
}

TEST_CASE("Impossible numeric selections and clocks cannot enter association arithmetic")
{
    auto state = face_lock::makeLock(anchorOf(face(500)), Crop);
    SECTION("Infinite anchor")
    {
        state.lastAnchor.centerX = std::numeric_limits<double>::infinity();
    }
    SECTION("Subpixel-zero anchor")
    {
        state.lastAnchor.width = std::numeric_limits<double>::denorm_min();
    }
    SECTION("Negative size")
    {
        state.sizeX = -1;
    }
    SECTION("Unbounded offset")
    {
        state.offsetX = std::numeric_limits<double>::max();
    }
    CHECK_FALSE(validSelection(Source, state, Bounds));
    CHECK_THROWS_AS(Association(Source, state, Bounds), std::invalid_argument);
}

TEST_CASE("The first fresh detection can start at monotonic zero without a settling delay")
{
    const auto state = face_lock::makeLock(anchorOf(face(500)), Crop);
    Association policy{Source, state, Bounds};
    const std::array boxes{face(500)};
    CHECK(policy.advance({Source, 1, 0.0}, 0.0, DetectionStatus::Completed, boxes).action == Action::Accepted);
    CHECK(policy.advance({Source, 2, 0.0}, 0.0, DetectionStatus::Completed, boxes).action == Action::Ignored);
}
