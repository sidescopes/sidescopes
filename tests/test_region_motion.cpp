#include <catch2/catch_test_macros.hpp>

#include "app/region_motion.h"

namespace sidescopes {

namespace {

RegionMotionStep step(RegionMotionTracker& tracker, bool regionChanged, bool windowMoving, double now)
{
    return tracker.update(RegionMotionInputs{regionChanged, windowMoving, now});
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

}  // namespace sidescopes
