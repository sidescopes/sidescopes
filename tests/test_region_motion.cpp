#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <optional>

#include "app/region_geometry.h"
#include "app/region_motion.h"

namespace sidescopes {

namespace {

RegionMotionStep step(RegionMotionTracker& tracker, bool regionChanged, bool windowMoving, double now,
                      double travel = 0.0)
{
    return tracker.update(RegionMotionInputs{regionChanged, windowMoving, now, travel});
}

// One step of a hand moving the region at a given speed, in per cent of the
// display a second, taken every @p interval seconds.
RegionMotionStep moveAt(RegionMotionTracker& tracker, double speed, double& clock, double interval = 0.05)
{
    clock += interval;

    return step(tracker, true, false, clock, speed * interval);
}

}  // namespace

TEST_CASE("A region nothing is moving reports still")
{
    RegionMotionTracker tracker;

    const RegionMotionStep first = step(tracker, false, false, 0.0);
    CHECK(first.motion == RegionMotion::Still);
    CHECK_FALSE(first.changed);
    CHECK(step(tracker, false, false, 10.0).motion == RegionMotion::Still);
}

TEST_CASE("The user moving the region reports a drag, and holds it between steps")
{
    // A drag arrives as a burst of changes with gaps between them, so the
    // state has to survive the gaps: otherwise one pass in the middle of a
    // drag is computed at full detail for nothing.
    RegionMotionTracker tracker;

    const RegionMotionStep first = step(tracker, true, false, 1.0);
    CHECK(first.motion == RegionMotion::Dragged);
    CHECK(first.changed);

    const RegionMotionStep between = step(tracker, false, false, 1.0 + RegionSettleSeconds / 2.0);
    CHECK(between.motion == RegionMotion::Dragged);
    CHECK_FALSE(between.changed);
}

TEST_CASE("A dragged region goes still once it has sat still, and says so")
{
    RegionMotionTracker tracker;
    CHECK(step(tracker, true, false, 1.0).motion == RegionMotion::Dragged);

    const RegionMotionStep settled = step(tracker, false, false, 1.0 + RegionSettleSeconds);
    CHECK(settled.motion == RegionMotion::Still);
    // The settings have to carry it: a region that stopped moving stops
    // dirtying them, so nothing else would take the detail back up.
    CHECK(settled.changed);
}

TEST_CASE("A window carrying the region reports it as carried, not dragged")
{
    // The whole point of the split: the user's hand is on the window, so the
    // region is in transit and analysis stops rather than coarsening.
    RegionMotionTracker tracker;

    const RegionMotionStep carried = step(tracker, true, true, 1.0);
    CHECK(carried.motion == RegionMotion::Carried);
    CHECK(carried.changed);
    CHECK(step(tracker, false, true, 1.5).motion == RegionMotion::Carried);
    CHECK(step(tracker, true, true, 2.0).motion == RegionMotion::Carried);
}

TEST_CASE("Carrying outranks a drag already in flight")
{
    RegionMotionTracker tracker;
    CHECK(step(tracker, true, false, 1.0).motion == RegionMotion::Dragged);
    CHECK(step(tracker, true, true, 1.0 + RegionSettleSeconds / 2.0).motion == RegionMotion::Carried);
}

TEST_CASE("A window that lands goes straight back to still")
{
    // The window's own settle time is shorter than the region's, so counting
    // the carry as a drag would leave one coarse pass standing behind a landed
    // window - a blur that arrives after the movement has stopped.
    RegionMotionTracker tracker;
    CHECK(step(tracker, true, true, 1.0).motion == RegionMotion::Carried);
    CHECK(step(tracker, true, true, 1.1).motion == RegionMotion::Carried);

    const RegionMotionStep landed = step(tracker, false, false, 1.15);
    CHECK(landed.motion == RegionMotion::Still);
    CHECK(landed.changed);
}

TEST_CASE("A drag started before a carry does not outlive it")
{
    RegionMotionTracker tracker;
    CHECK(step(tracker, true, false, 1.0).motion == RegionMotion::Dragged);
    CHECK(step(tracker, true, true, 1.05).motion == RegionMotion::Carried);
    // Well inside the region's settle time, but the last thing the user's own
    // hand moved was a fifth of a second ago and the window has landed since.
    CHECK(step(tracker, false, false, 1.30).motion == RegionMotion::Still);
}

TEST_CASE("A drag straight after a carry is a drag")
{
    // Dropping a window and immediately taking the border's band must read as
    // the user's own gesture, not as the tail of the window's.
    RegionMotionTracker tracker;
    CHECK(step(tracker, true, true, 1.0).motion == RegionMotion::Carried);
    CHECK(step(tracker, true, false, 1.1).motion == RegionMotion::Dragged);
}

TEST_CASE("A region scanned across a picture is read, and one thrown is not")
{
    // The owner's two gestures, and the whole reason the speed is measured at
    // all: a region walked over a sky hunting blown highlights is read while it
    // moves, and one flung from face to face is not.
    RegionMotionTracker scanning;
    double clock = 1.0;
    for (int move = 0; move < 6; ++move) {
        const RegionMotionStep taken = moveAt(scanning, ScannedSpeedPercent * 0.6, clock);
        CHECK(taken.motion == RegionMotion::Dragged);
        CHECK_FALSE(taken.thrown);
    }

    RegionMotionTracker throwing;
    clock = 1.0;
    // The first change of a gesture is measured against whenever the last one
    // was, which is long ago, so a throw is known from the second.
    moveAt(throwing, ThrownSpeedPercent * 2.0, clock);
    const RegionMotionStep thrown = moveAt(throwing, ThrownSpeedPercent * 2.0, clock);
    CHECK(thrown.motion == RegionMotion::Dragged);
    CHECK(thrown.thrown);
    CHECK(thrown.changed);
}

TEST_CASE("One gesture stays in one regime")
{
    // A hand does not hold a speed, and a threshold with no hysteresis would
    // flick the scopes on and off through the middle of a single throw.
    RegionMotionTracker tracker;
    double clock = 1.0;
    moveAt(tracker, ThrownSpeedPercent * 2.0, clock);
    REQUIRE(moveAt(tracker, ThrownSpeedPercent * 2.0, clock).thrown);

    // Wavering below the speed that starts a throw does not end one.
    const double between = (ThrownSpeedPercent + ScannedSpeedPercent) / 2.0;
    CHECK(moveAt(tracker, between, clock).thrown);
    CHECK(moveAt(tracker, between, clock).thrown);

    // Slowing right down to a scan does.
    const RegionMotionStep slowed = moveAt(tracker, ScannedSpeedPercent * 0.5, clock);
    CHECK_FALSE(slowed.thrown);
    CHECK(slowed.changed);
    CHECK(ScannedSpeedPercent < ThrownSpeedPercent);
}

TEST_CASE("A throw holds through the gaps in the gesture that made it")
{
    // A drag reaches the tracker as a burst of changes with gaps between them,
    // and the gaps are the settings cadence rather than the hand stopping.
    RegionMotionTracker tracker;
    double clock = 1.0;
    moveAt(tracker, ThrownSpeedPercent * 2.0, clock);
    REQUIRE(moveAt(tracker, ThrownSpeedPercent * 2.0, clock).thrown);

    const RegionMotionStep between = step(tracker, false, false, clock + RegionSettleSeconds / 2.0);
    CHECK(between.motion == RegionMotion::Dragged);
    CHECK(between.thrown);
    CHECK_FALSE(between.changed);
}

TEST_CASE("A thrown region that lands is read again, once")
{
    // The release has to restore the sharp trace by itself, and exactly once:
    // a region that stopped moving stops dirtying the settings, so nothing else
    // would let analysis go again.
    RegionMotionTracker tracker;
    double clock = 1.0;
    moveAt(tracker, ThrownSpeedPercent * 2.0, clock);
    REQUIRE(moveAt(tracker, ThrownSpeedPercent * 2.0, clock).thrown);

    const RegionMotionStep landed = step(tracker, false, false, clock + RegionSettleSeconds);
    CHECK(landed.motion == RegionMotion::Still);
    CHECK_FALSE(landed.thrown);
    CHECK(landed.changed);
    CHECK_FALSE(step(tracker, false, false, clock + 1.0).changed);
}

TEST_CASE("A jump that is not part of a gesture is not a throw")
{
    // The picker's hover preview: crossing from one window candidate to the
    // next moves the previewed region a third of the screen in one step, and
    // arrives whenever the pointer gets there. Reading that as a throw would
    // hold the very preview the user is hovering to see. A speed needs two
    // changes of one gesture, and a gap longer than the settle says the last
    // gesture is over.
    RegionMotionTracker tracker;
    double clock = 1.0;
    step(tracker, true, false, clock, 0.0);
    clock += RegionSettleSeconds + 0.01;
    const RegionMotionStep jumped = step(tracker, true, false, clock, 35.0);
    CHECK(jumped.motion == RegionMotion::Dragged);
    CHECK_FALSE(jumped.thrown);
}

TEST_CASE("Travel is the fastest edge, which is what the hand is holding")
{
    // The measure the speed above is taken from. The centre would do for a
    // region being moved and halve the speed of one being drawn, because a
    // drag from a corner pins two edges and throws the other two.
    const RegionOfInterest start{10.0, 10.0, 20.0, 20.0};
    const RegionOfInterest moved{18.0, 13.0, 28.0, 23.0};
    CHECK(regionTravelPercent(start, moved) == Catch::Approx(8.0));

    const RegionOfInterest drawn{10.0, 10.0, 34.0, 26.0};
    CHECK(regionTravelPercent(start, drawn) == Catch::Approx(14.0));

    // A region appearing or going away has not moved from anywhere.
    CHECK(regionTravelPercent(std::nullopt, start) == 0.0);
    CHECK(regionTravelPercent(start, std::nullopt) == 0.0);
    CHECK(regionTravelPercent(start, start) == 0.0);
}

TEST_CASE("A window carrying the region is never reported as thrown")
{
    // A carry is already held outright, and reporting it as a throw as well
    // would make the two indistinguishable to anything reading the step.
    RegionMotionTracker tracker;
    double clock = 1.0;
    moveAt(tracker, ThrownSpeedPercent * 2.0, clock);
    REQUIRE(moveAt(tracker, ThrownSpeedPercent * 2.0, clock).thrown);

    const RegionMotionStep carried = step(tracker, true, true, clock + 0.05, 20.0);
    CHECK(carried.motion == RegionMotion::Carried);
    CHECK_FALSE(carried.thrown);
}

}  // namespace sidescopes
