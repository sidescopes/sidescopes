#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
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
    CHECK(policy.remap(moved, shifted, Bounds, 0, 0).action == Action::Held);
    CHECK(policy.current().anchor.centerX == 510);
    CHECK(policy.tick(moved, 10.10).reason == Reason::Ambiguous);
    CHECK(policy.tick(moved, 10.46).action == Action::OrdinaryAttached);
    const Context edited{3, Source.epoch, Source.display};
    CHECK_FALSE(policy.remap(edited, shifted, Bounds, 0, 0).following);
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
    const auto remapped = policy.remap(next, saved, Bounds, 0, 0);
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

namespace {

Association withRecentRival()
{
    auto policy = selected();
    REQUIRE(observe(policy, 2, 10.04, {face(500), face(800)}).action == Action::Accepted);
    return policy;
}

Decision afterMiss(Association& policy, Context context = Source)
{
    REQUIRE(policy.advance({context, 3, 10.08}, 10.08, DetectionStatus::Completed, {}).action == Action::Held);
    const std::array boxes{face(700)};
    return policy.advance({context, 4, 10.20}, 10.20, DetectionStatus::Completed, boxes);
}

LockRect xywh(double x, double y, double width, double height)
{
    return {x, y, x + width, y + height};
}

}  // namespace

TEST_CASE("A recent rival cannot become the selected face as a miss widens the gate", "[recent-rival]")
{
    // Consecutive detector observations from an overlap: the selected face is
    // the second box until frame 94 and is absent in frames 95 through 98.
    const LockRect bounds{0, 0, 640, 480};
    const auto seed = xywh(228, 157, 76, 121);
    Association policy{{Source, 93, 92.0 / 25}, seed, xywh(240, 175, 48, 70), bounds, 92.0 / 25};
    auto first = observe(policy, 94, 93.0 / 25, {xywh(313, 151, 90, 116), xywh(231, 156, 75, 124)});
    REQUIRE(first.action == Action::Accepted);
    REQUIRE(first.selectedBox == 1);
    auto last = observe(policy, 95, 94.0 / 25, {xywh(307, 150, 90, 116), xywh(237, 145, 72, 134)});
    REQUIRE(last.action == Action::Accepted);
    REQUIRE(last.selectedBox == 1);
    CHECK(observe(policy, 96, 95.0 / 25, {xywh(305, 150, 89, 114)}).reason == Reason::NoPlausibleMatch);
    CHECK(observe(policy, 97, 96.0 / 25, {xywh(302, 150, 91, 117)}).reason == Reason::NoPlausibleMatch);
    CHECK(observe(policy, 98, 97.0 / 25, {xywh(299, 150, 91, 119)}).reason == Reason::NoPlausibleMatch);
    const auto guarded = observe(policy, 99, 98.0 / 25, {xywh(298, 146, 86, 117)});
    REQUIRE(guarded.action == Action::Held);
    CHECK(guarded.reason == Reason::Ambiguous);
    CHECK_FALSE(guarded.selectedBox);
    checkCrop(guarded, last.crop);
    CHECK(guarded.evidenceSourceSeconds == 94.0 / 25);
    CHECK(observe(policy, 100, 99.0 / 25, {xywh(294, 148, 89, 118)}).reason == Reason::Ambiguous);
    CHECK(policy.tick(Source, 4.21).action == Action::OrdinaryAttached);
}

TEST_CASE("Single-face acceleration stop reversal and zoom retain immediate acceptance", "[recent-rival]")
{
    auto policy = selected();
    const std::array positions{510., 540., 590., 640., 640., 600., 555., 570., 610.};
    const std::array widths{200., 200., 210., 220., 220., 200., 185., 195., 215.};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const auto result = observe(policy, i + 2, 10.0 + (i + 1) * .05, {face(positions[i], 500, widths[i])});
        REQUIRE(result.action == Action::Accepted);
        CHECK(result.anchor.centerX == positions[i]);
        CHECK(result.anchor.width == widths[i]);
    }
}

TEST_CASE("A departed rival does not block a continuously followed face", "[recent-rival]")
{
    auto policy = withRecentRival();
    for (int i = 1; i <= 9; ++i) {
        const double position = 500 + i * 40;
        const auto followed = observe(policy, i + 2, 10.04 + i * .04, {face(position)});
        REQUIRE(followed.action == Action::Accepted);
        CHECK(followed.anchor.centerX == position);
    }
}

TEST_CASE("Brief selected misses keep negative evidence without renewing positive evidence", "[recent-rival]")
{
    auto policy = withRecentRival();
    const auto decision = afterMiss(policy);
    CHECK(decision.reason == Reason::Ambiguous);
    CHECK(decision.action == Action::Held);
    CHECK(decision.evidenceSourceSeconds == 10.04);
    checkCrop(decision, Crop);
    CHECK(policy.tick(Source, 10.49).action == Action::OrdinaryAttached);
}

TEST_CASE("A returning selected face can recover beside the same distant rival", "[recent-rival]")
{
    auto policy = withRecentRival();
    REQUIRE(observe(policy, 3, 10.08, {}).action == Action::Held);
    const auto result = observe(policy, 4, 10.12, {face(800), face(515)});
    REQUIRE(result.action == Action::Accepted);
    CHECK(result.selectedBox == 1);
    CHECK(result.anchor.centerX == 515);
}

TEST_CASE("Rival evidence expires on source time after healthy following", "[recent-rival]")
{
    auto policy = withRecentRival();
    for (int i = 1; i <= 11; ++i) {
        REQUIRE(observe(policy, i + 2, 10.04 + i * .04, {face(500)}).action == Action::Accepted);
    }
    const auto result = observe(policy, 14, 10.64, {face(700)});
    CHECK(result.action == Action::Accepted);
    CHECK(result.anchor.centerX == 700);
}

TEST_CASE("Ignored observations cannot replace or expire recent rival evidence", "[recent-rival]")
{
    auto policy = withRecentRival();
    const std::array unrelated{face(500), face(1000)};
    SECTION("Foreign source")
    {
        CHECK(policy.advance({{1, 99, 3}, 10, 99}, 99, DetectionStatus::Completed, unrelated).reason ==
              Reason::StaleContext);
    }
    SECTION("Foreign generation")
    {
        CHECK(policy.advance({{2, 9, 3}, 10, 99}, 99, DetectionStatus::Completed, unrelated).reason ==
              Reason::StaleContext);
    }
    SECTION("Foreign display")
    {
        CHECK(policy.advance({{1, 9, 4}, 10, 99}, 99, DetectionStatus::Completed, unrelated).reason ==
              Reason::StaleContext);
    }
    SECTION("Duplicate sequence")
    {
        CHECK(policy.advance({Source, 2, 99}, 99, DetectionStatus::Completed, unrelated).reason ==
              Reason::DuplicateOrOlderFrame);
    }
    SECTION("Equal receipt time")
    {
        CHECK(policy.advance({Source, 3, 10.04}, 10.05, DetectionStatus::Completed, unrelated).reason ==
              Reason::NonIncreasingSourceTime);
    }
    SECTION("Invalid clock")
    {
        CHECK(policy.advance({Source, 3, 10.05}, 10.03, DetectionStatus::Completed, unrelated).reason ==
              Reason::InvalidClock);
    }
    CHECK(afterMiss(policy).reason == Reason::Ambiguous);
}

TEST_CASE("Unavailable and malformed detections do not erase negative evidence", "[recent-rival]")
{
    auto policy = withRecentRival();
    SECTION("Native failure")
    {
        CHECK(policy.advance({Source, 3, 10.08}, 10.08, DetectionStatus::Failed, {}).reason == Reason::NativeFailure);
    }
    SECTION("Native status unavailable")
    {
        CHECK(policy.advance({Source, 3, 10.08}, 10.08, DetectionStatus::Unavailable, {}).reason ==
              Reason::NativeStatusUnavailable);
    }
    SECTION("Malformed boxes")
    {
        const std::array invalid{LockRect{10, 10, 5, 5}};
        CHECK(policy.advance({Source, 3, 10.08}, 10.08, DetectionStatus::Completed, invalid).reason ==
              Reason::InvalidDetection);
    }
    CHECK(observe(policy, 4, 10.20, {face(700)}).reason == Reason::Ambiguous);
}

TEST_CASE("Manual crop edits preserve recent rival evidence and do not refresh its clock", "[recent-rival]")
{
    auto policy = withRecentRival();
    const Context edited{2, Source.epoch, Source.display};
    const LockRect crop{460, 390, 540, 430};
    REQUIRE(policy.rebindCrop(edited, crop).action == Action::Held);
    const auto result = afterMiss(policy, edited);
    CHECK(result.reason == Reason::Ambiguous);
    checkCrop(result, crop);
    CHECK(result.evidenceSourceSeconds == 10.04);
}

TEST_CASE("Proven stream resume preserves recent rival memory and monotonic source time", "[recent-rival]")
{
    auto policy = withRecentRival();
    const Context resumed{2, Source.epoch + 1, Source.display};
    const auto crop = face_lock::makeLock(anchorOf(face(500)), Crop);
    REQUIRE(policy.remap(resumed, crop, Bounds, 0, 0, true).action == Action::Held);
    CHECK(policy.advance({resumed, 1, 10.04}, 10.05, DetectionStatus::Completed, {}).reason ==
          Reason::NonIncreasingSourceTime);
    REQUIRE(policy.advance({resumed, 1, 10.08}, 10.08, DetectionStatus::Completed, {}).action == Action::Held);
    const std::array boxes{face(700)};
    CHECK(policy.advance({resumed, 2, 10.20}, 10.20, DetectionStatus::Completed, boxes).reason == Reason::Ambiguous);
}

TEST_CASE("Parent translation carries rival positions independently of saved crop offsets", "[recent-rival]")
{
    auto policy = withRecentRival();
    const Context moved{2, Source.epoch, Source.display};
    const LockRect bounds{100, 50, 4196, 2210};
    const auto crop = face_lock::makeLock(anchorOf(face(600, 550)), {550, 430, 650, 490});
    REQUIRE(policy.remap(moved, crop, bounds, 100, 50).action == Action::Held);
    REQUIRE(policy.advance({moved, 3, 10.08}, 10.08, DetectionStatus::Completed, {}).action == Action::Held);
    const std::array boxes{face(800, 550)};
    CHECK(policy.advance({moved, 4, 10.20}, 10.20, DetectionStatus::Completed, boxes).reason == Reason::Ambiguous);
}

TEST_CASE("A stale saved anchor does not translate rival evidence within unchanged bounds", "[recent-rival]")
{
    auto policy = selected();
    REQUIRE(observe(policy, 2, 10.04, {face(520), face(700)}).action == Action::Accepted);
    const Context edited{2, Source.epoch, Source.display};
    const auto saved = face_lock::makeLock(anchorOf(face(500)), Crop);
    REQUIRE(policy.remap(edited, saved, Bounds, 0, 0).action == Action::Held);
    const std::array boxes{face(615)};
    CHECK(policy.advance({edited, 3, 10.20}, 10.20, DetectionStatus::Completed, boxes).action == Action::Accepted);
}

TEST_CASE("Parent resize cannot erase recent rival evidence", "[recent-rival]")
{
    auto policy = withRecentRival();
    const Context resized{2, Source.epoch, Source.display};
    const auto crop = face_lock::makeLock(anchorOf(face(500)), Crop);
    REQUIRE(policy.remap(resized, crop, {0, 0, 4000, 2100}, 0, 0).action == Action::Held);
    CHECK(afterMiss(policy, resized).reason == Reason::Ambiguous);
}

TEST_CASE("Source retirement stays retired and a new explicit selection starts without rivals", "[recent-rival]")
{
    auto old = withRecentRival();
    const Context next{2, Source.epoch + 1, 4};
    const auto crop = face_lock::makeLock(anchorOf(face(500)), Crop);
    REQUIRE(old.retire(next, crop, Bounds).action == Action::OrdinaryAttached);
    const std::array boxes{face(700)};
    CHECK(old.advance({next, 1, 10.20}, 10.20, DetectionStatus::Completed, boxes).action == Action::OrdinaryAttached);
    Association fresh{{next, 1, 10.04}, face(500), Crop, Bounds, 10.04};
    CHECK(fresh.advance({next, 2, 10.20}, 10.20, DetectionStatus::Completed, boxes).action == Action::Accepted);
}

TEST_CASE("Bounded rival capacity refuses crowding without dropping a known competitor", "[recent-rival]")
{
    Association policy{{Source, 1, 10}, face(1000, 1000), {950, 950, 1050, 1050}, Bounds, 10};
    std::vector<LockRect> boxes{face(1000, 1000)};
    for (int i = 0; i < 9; ++i) {
        const double angle = i * 6.283185307179586 / 9;
        boxes.push_back(face(1000 + 400 * std::cos(angle), 1000 + 400 * std::sin(angle)));
    }
    const auto decision = observe(policy, 2, 10.04, boxes);
    CHECK(decision.action == Action::Held);
    CHECK(decision.reason == Reason::Ambiguous);
    CHECK(decision.evidenceSourceSeconds == 10);
}

TEST_CASE("A different visible rival cannot replace a recently missing rival", "[recent-rival]")
{
    auto policy = withRecentRival();
    REQUIRE(observe(policy, 3, 10.08, {face(500), face(200)}).action == Action::Accepted);
    REQUIRE(observe(policy, 4, 10.12, {}).action == Action::Held);
    CHECK(observe(policy, 5, 10.24, {face(700)}).reason == Reason::Ambiguous);
}

TEST_CASE("Presentation ticks cannot renew or clear rival observations", "[recent-rival]")
{
    auto policy = withRecentRival();
    REQUIRE(policy.tick(Source, 10.08).reason == Reason::Waiting);
    REQUIRE(policy.tick(Source, 10.12).reason == Reason::Waiting);
    REQUIRE(observe(policy, 3, 10.16, {}).action == Action::Held);
    CHECK(observe(policy, 4, 10.20, {face(700)}).reason == Reason::Ambiguous);
    CHECK(policy.current().evidenceSourceSeconds == 10.04);
}

TEST_CASE("An expired native result cannot replace recent rival observations", "[recent-rival]")
{
    auto policy = withRecentRival();
    const std::array unrelated{face(500), face(1000)};
    CHECK(policy.advance({Source, 3, 10.10}, 10.31, DetectionStatus::Completed, unrelated).reason ==
          Reason::ExpiredResult);
    CHECK(observe(policy, 4, 10.32, {face(700)}).reason == Reason::Ambiguous);
}

TEST_CASE("Visible departing rivals permit admitted lower-cadence face motion", "[recent-rival]")
{
    auto policy = withRecentRival();
    const auto moved = observe(policy, 3, 10.20, {face(700), face(1000)});
    REQUIRE(moved.action == Action::Accepted);
    CHECK(moved.selectedBox == 0);
    CHECK(moved.anchor.centerX == 700);
    CHECK(observe(policy, 4, 10.36, {face(900), face(1200)}).action == Action::Accepted);
}

TEST_CASE("Distinct current rivals preserve recovery protection in either detector order", "[recent-rival]")
{
    const bool reversed = GENERATE(false, true);
    Association policy{{Source, 1, 10}, face(200, 200, 80), {180, 180, 220, 220}, Bounds, 10};
    std::vector<LockRect> boxes{face(200, 200, 80), face(320, 200, 80), face(360, 200, 80)};
    if (reversed) {
        std::swap(boxes[1], boxes[2]);
    }
    REQUIRE(observe(policy, 2, 10.04, boxes).action == Action::Accepted);
    REQUIRE(observe(policy, 3, 10.08, {}).action == Action::Held);
    const auto survivor = observe(policy, 4, 10.20, {face(280, 200, 80)});
    CHECK(survivor.action == Action::Held);
    CHECK(survivor.reason == Reason::Ambiguous);
    CHECK(survivor.evidenceSourceSeconds == 10.04);
    checkCrop(survivor, {180, 180, 220, 220});
}

TEST_CASE("Rival batches have identical recovery effects under every box permutation", "[recent-rival]")
{
    std::array<int, 3> order{0, 1, 2};
    do {
        for (int x = 255; x <= 280; x += 5) {
            Association policy{{Source, 1, 10}, face(200, 200, 80), {180, 180, 220, 220}, Bounds, 10};
            std::vector<LockRect> boxes{face(200, 200, 80)};
            for (const int index : order) {
                boxes.push_back(face(310 + index * 30, 200, 80));
            }
            REQUIRE(observe(policy, 2, 10.04, boxes).action == Action::Accepted);
            REQUIRE(observe(policy, 3, 10.08, {}).action == Action::Held);
            const auto result = observe(policy, 4, 10.20, {face(x, 200, 80)});
            INFO(x);
            // At 255/260 the selected prior still explains the return at
            // least as well; 265..280 are substantially nearer a known rival.
            const bool rivalExplainsReturn = x >= 265;
            CHECK(result.action == (rivalExplainsReturn ? Action::Held : Action::Accepted));
            CHECK(result.reason == (rivalExplainsReturn ? Reason::Ambiguous : Reason::Followed));
            CHECK(result.evidenceSourceSeconds == (rivalExplainsReturn ? 10.04 : 10.20));
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_CASE("Invalid parent translations cannot change rival memory", "[clipped-parent]")
{
    const double invalid = GENERATE(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity());
    const bool horizontal = GENERATE(false, true);
    auto policy = withRecentRival();
    const Context moved{2, Source.epoch, Source.display};
    const auto crop = face_lock::makeLock(anchorOf(face(500)), Crop);
    REQUIRE(policy.remap(moved, crop, Bounds, horizontal ? invalid : 0, horizontal ? 0 : invalid).action ==
            Action::Ignored);
    CHECK(afterMiss(policy).reason == Reason::Ambiguous);
}
