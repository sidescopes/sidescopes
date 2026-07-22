#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "app/face_lock.h"

using namespace sidescopes;
using Catch::Matchers::WithinAbs;

namespace {

FaceAnchor anchorAt(double x, double y, double width)
{
    return FaceAnchor{x, y, width};
}

FaceLockState foreheadLock()
{
    // A face at 500,300 sized 200, cropped to a forehead-like rectangle.
    return face_lock::makeLock(anchorAt(500.0, 300.0, 200.0), LockRect{450.0, 250.0, 550.0, 350.0});
}

}  // namespace

TEST_CASE("A fresh lock reproduces its crop at the picked anchor")
{
    const auto anchor = anchorAt(500.0, 300.0, 200.0);
    const auto lock = face_lock::makeLock(anchor, LockRect{450.0, 250.0, 550.0, 350.0});

    const auto mapped = face_lock::mapRegion(lock, anchor);
    CHECK_THAT(mapped.left, WithinAbs(450.0, 1e-9));
    CHECK_THAT(mapped.top, WithinAbs(250.0, 1e-9));
    CHECK_THAT(mapped.right, WithinAbs(550.0, 1e-9));
    CHECK_THAT(mapped.bottom, WithinAbs(350.0, 1e-9));
}

TEST_CASE("The crop pans and zooms with the anchor")
{
    // A forehead crop: above the face centre, half its width.
    const auto lock = face_lock::makeLock(anchorAt(500.0, 300.0, 200.0), LockRect{450.0, 180.0, 550.0, 240.0});

    // The photo pans right by 300 and zooms to twice the size.
    const auto mapped = face_lock::mapRegion(lock, anchorAt(800.0, 300.0, 400.0));
    CHECK_THAT(mapped.left, WithinAbs(700.0, 1e-9));
    CHECK_THAT(mapped.top, WithinAbs(60.0, 1e-9));
    CHECK_THAT(mapped.right, WithinAbs(900.0, 1e-9));
    CHECK_THAT(mapped.bottom, WithinAbs(180.0, 1e-9));
}

TEST_CASE("A border edit rebinds the crop from the last anchor")
{
    auto lock = face_lock::makeLock(anchorAt(500.0, 300.0, 200.0), LockRect{400.0, 200.0, 600.0, 400.0});

    face_lock::rebindCrop(lock, LockRect{480.0, 220.0, 520.0, 260.0});

    const auto mapped = face_lock::mapRegion(lock, anchorAt(500.0, 300.0, 200.0));
    CHECK_THAT(mapped.left, WithinAbs(480.0, 1e-9));
    CHECK_THAT(mapped.top, WithinAbs(220.0, 1e-9));
    CHECK_THAT(mapped.right, WithinAbs(520.0, 1e-9));
    CHECK_THAT(mapped.bottom, WithinAbs(260.0, 1e-9));
}

TEST_CASE("A moved face is adopted after two agreeing probes")
{
    auto lock = foreheadLock();

    CHECK_FALSE(face_lock::decide(lock, {anchorAt(560.0, 300.0, 200.0)}).adopt);
    CHECK(face_lock::decide(lock, {anchorAt(560.0, 300.0, 200.0)}).adopt);
    CHECK_THAT(lock.lastAnchor.centerX, WithinAbs(560.0, 1e-9));
}

TEST_CASE("A pan in progress freezes the region")
{
    auto lock = foreheadLock();

    // Successive probes see the face at ever-new places: never adopted.
    CHECK(face_lock::decide(lock, {anchorAt(560.0, 300.0, 200.0)}).reason == "unsettled");
    CHECK(face_lock::decide(lock, {anchorAt(650.0, 300.0, 200.0)}).reason == "unsettled");
    CHECK(face_lock::decide(lock, {anchorAt(740.0, 300.0, 200.0)}).reason == "unsettled");
    CHECK_THAT(lock.lastAnchor.centerX, WithinAbs(500.0, 1e-9));

    // The pan ends: one agreeing pair, one clean snap.
    CHECK(face_lock::decide(lock, {anchorAt(750.0, 300.0, 200.0)}).adopt);
    CHECK_THAT(lock.lastAnchor.centerX, WithinAbs(750.0, 1e-9));
}

TEST_CASE("A slow pan is followed step by step")
{
    auto lock = foreheadLock();

    // Steps within the agreement slack keep the pending streak alive, so
    // each probe beyond the micro threshold adopts.
    CHECK(face_lock::decide(lock, {anchorAt(525.0, 300.0, 200.0)}).adopt);
    CHECK(face_lock::decide(lock, {anchorAt(550.0, 300.0, 200.0)}).adopt);
    CHECK_THAT(lock.lastAnchor.centerX, WithinAbs(550.0, 1e-9));
}

TEST_CASE("No faces holds the region")
{
    auto lock = foreheadLock();

    CHECK_FALSE(face_lock::decide(lock, {}).adopt);
    CHECK_THAT(lock.lastAnchor.centerX, WithinAbs(500.0, 1e-9));
}

TEST_CASE("A thumbnail-sized face never captures the lock")
{
    auto lock = foreheadLock();

    CHECK_FALSE(face_lock::decide(lock, {anchorAt(500.0, 300.0, 60.0)}).adopt);
    CHECK_FALSE(face_lock::decide(lock, {anchorAt(500.0, 300.0, 500.0)}).adopt);
}

TEST_CASE("Two nearby faces freeze the lock")
{
    auto lock = foreheadLock();

    const auto decision = face_lock::decide(lock, {anchorAt(450.0, 300.0, 200.0), anchorAt(550.0, 300.0, 190.0)});
    CHECK_FALSE(decision.adopt);
    CHECK(decision.reason == "ambiguous (2 rivals)");
}

TEST_CASE("Detector jitter is not adopted")
{
    auto lock = foreheadLock();

    CHECK_FALSE(face_lock::decide(lock, {anchorAt(504.0, 302.0, 203.0)}).adopt);
    const auto decision = face_lock::decide(lock, {anchorAt(504.0, 302.0, 203.0)});
    CHECK_FALSE(decision.adopt);
    CHECK(decision.reason == "micro-move");
    CHECK_THAT(lock.lastAnchor.centerX, WithinAbs(500.0, 1e-9));
}

TEST_CASE("Slow drift accumulates until it is adopted")
{
    auto lock = foreheadLock();

    // Each step agrees with the last sighting but is micro against the
    // adopted anchor, until the sum is not.
    CHECK_FALSE(face_lock::decide(lock, {anchorAt(508.0, 300.0, 200.0)}).adopt);
    CHECK(face_lock::decide(lock, {anchorAt(520.0, 300.0, 200.0)}).adopt);
    CHECK_THAT(lock.lastAnchor.centerX, WithinAbs(520.0, 1e-9));
}

TEST_CASE("A lost face widens the search after three misses")
{
    auto lock = foreheadLock();

    CHECK_FALSE(face_lock::searchingWide(lock));
    // The pan carried the face beyond the proximity gate.
    for (int probe = 0; probe < 3; ++probe) {
        const auto decision = face_lock::decide(lock, {anchorAt(1500.0, 700.0, 200.0)});
        CHECK_FALSE(decision.adopt);
    }
    CHECK(face_lock::searchingWide(lock));

    // Wide mode reaches it; adoption still takes two agreeing sightings.
    CHECK(face_lock::decide(lock, {anchorAt(1500.0, 700.0, 200.0)}).reason == "unsettled");
    const auto decision = face_lock::decide(lock, {anchorAt(1500.0, 700.0, 200.0)});
    CHECK(decision.adopt);
    CHECK(decision.reason == "reacquired");
    CHECK_FALSE(face_lock::searchingWide(lock));
    CHECK_THAT(lock.lastAnchor.centerX, WithinAbs(1500.0, 1e-9));
}

TEST_CASE("A zoomed face is reacquired at its new size")
{
    auto lock = face_lock::makeLock(anchorAt(500.0, 300.0, 200.0), LockRect{450.0, 180.0, 550.0, 240.0});

    // Zoom to 100%: three times the size, outside the tight size gate.
    const auto zoomed = anchorAt(600.0, 400.0, 600.0);
    for (int probe = 0; probe < 3; ++probe) {
        CHECK_FALSE(face_lock::decide(lock, {zoomed}).adopt);
    }
    CHECK(face_lock::searchingWide(lock));
    CHECK_FALSE(face_lock::decide(lock, {zoomed}).adopt);
    CHECK(face_lock::decide(lock, {zoomed}).adopt);

    // The forehead crop scales with the anchor width by construction.
    const auto mapped = face_lock::mapRegion(lock, lock.lastAnchor);
    CHECK_THAT(mapped.right - mapped.left, WithinAbs(300.0, 1e-9));
    CHECK_THAT(mapped.bottom - mapped.top, WithinAbs(180.0, 1e-9));
}

TEST_CASE("A face found back at the anchor ends the wide search without a jump")
{
    auto lock = foreheadLock();

    for (int probe = 0; probe < 3; ++probe) {
        CHECK_FALSE(face_lock::decide(lock, {}).adopt);
    }
    CHECK(face_lock::searchingWide(lock));

    CHECK(face_lock::decide(lock, {anchorAt(500.0, 300.0, 200.0)}).reason == "unsettled");
    const auto decision = face_lock::decide(lock, {anchorAt(500.0, 300.0, 200.0)});
    CHECK_FALSE(decision.adopt);
    CHECK(decision.reason == "micro-move");
    CHECK_FALSE(face_lock::searchingWide(lock));
}

TEST_CASE("A window translation carries the anchors")
{
    auto lock = foreheadLock();

    face_lock::translate(lock, 300.0, 120.0);

    // The face sits exactly where the translated anchor expects it: no
    // adoption needed, and certainly no lost lock.
    CHECK(face_lock::decide(lock, {anchorAt(800.0, 420.0, 200.0)}).reason == "micro-move");
    const auto mapped = face_lock::mapRegion(lock, lock.lastAnchor);
    CHECK_THAT(mapped.left, WithinAbs(750.0, 1e-9));
    CHECK_THAT(mapped.top, WithinAbs(370.0, 1e-9));
}

TEST_CASE("Hunting is flagged whenever the face is not confirmed")
{
    auto lock = foreheadLock();

    // A jump: hunting until the position is confirmed.
    CHECK(face_lock::decide(lock, {anchorAt(560.0, 300.0, 200.0)}).hunting);
    CHECK_FALSE(face_lock::decide(lock, {anchorAt(560.0, 300.0, 200.0)}).hunting);

    // Sitting still is confirmed, not hunting.
    CHECK_FALSE(face_lock::decide(lock, {anchorAt(560.0, 300.0, 200.0)}).hunting);

    // Gone is hunting.
    CHECK(face_lock::decide(lock, {}).hunting);
}

TEST_CASE("Rival faces are uncertainty: the border hides and the clock runs")
{
    auto lock = foreheadLock();
    const std::vector<FaceAnchor> rivals{anchorAt(450.0, 300.0, 200.0), anchorAt(550.0, 300.0, 190.0)};

    CHECK(face_lock::decide(lock, rivals).hunting);

    // Persistent rivalry ends the lock instead of guessing.
    for (int probe = 0; probe < 15; ++probe) {
        (void)face_lock::decide(lock, rivals);
    }
    CHECK(face_lock::givenUp(lock));
}

TEST_CASE("A face seen but still moving never advances the give-up clock")
{
    auto lock = foreheadLock();

    for (int probe = 0; probe < 10; ++probe) {
        (void)face_lock::decide(lock, {});
    }
    // The face keeps appearing at disagreeing spots: evidence it exists,
    // no certainty where - the lock neither adopts nor gives up.
    for (int probe = 0; probe < 10; ++probe) {
        const double x = (probe % 2 == 0) ? 900.0 : 1500.0;
        CHECK(face_lock::decide(lock, {anchorAt(x, 500.0, 200.0)}).reason == "unsettled");
    }
    CHECK_FALSE(face_lock::givenUp(lock));

    // The last alternating sighting was at 1500; settling at 900 needs its
    // own agreeing pair.
    CHECK_FALSE(face_lock::decide(lock, {anchorAt(900.0, 500.0, 200.0)}).adopt);
    CHECK(face_lock::decide(lock, {anchorAt(900.0, 500.0, 200.0)}).adopt);
}

TEST_CASE("Only a side-clipped box is distrusted")
{
    const LockRect bounds{0.0, 0.0, 1000.0, 800.0};

    // Comfortably inside.
    CHECK(face_lock::trustworthyBox(LockRect{400.0, 300.0, 600.0, 500.0}, bounds));
    // Flush against the left or right edge: the width lies.
    CHECK_FALSE(face_lock::trustworthyBox(LockRect{0.0, 300.0, 200.0, 500.0}, bounds));
    CHECK_FALSE(face_lock::trustworthyBox(LockRect{802.0, 300.0, 998.0, 500.0}, bounds));
    // Flush against the bottom: the width is intact - a face near the
    // window's bottom edge stays trackable.
    CHECK(face_lock::trustworthyBox(LockRect{400.0, 610.0, 600.0, 800.0}, bounds));
    // A portrait filling the window: near every edge, still trusted.
    CHECK(face_lock::trustworthyBox(LockRect{10.0, 5.0, 990.0, 795.0}, bounds));
}

TEST_CASE("A face missing for long enough gives the lock up")
{
    auto lock = foreheadLock();

    for (int probe = 0; probe < 16; ++probe) {
        CHECK_FALSE(face_lock::givenUp(lock));
        (void)face_lock::decide(lock, {});
    }
    CHECK(face_lock::givenUp(lock));
}

TEST_CASE("A reappearing face rewinds the give-up clock fully")
{
    auto lock = foreheadLock();

    for (int probe = 0; probe < 10; ++probe) {
        (void)face_lock::decide(lock, {});
    }
    (void)face_lock::decide(lock, {anchorAt(500.0, 300.0, 200.0)});
    (void)face_lock::decide(lock, {anchorAt(500.0, 300.0, 200.0)});
    CHECK_FALSE(face_lock::searchingWide(lock));

    for (int probe = 0; probe < 10; ++probe) {
        (void)face_lock::decide(lock, {});
    }
    CHECK_FALSE(face_lock::givenUp(lock));
}
