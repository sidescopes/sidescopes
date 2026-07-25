#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/frame_pacing.h"

namespace sidescopes {
namespace {

// A window showing the scopes with nothing in the way.
constexpr VisibilityInputs InSight{};

VisibilityInputs hidden()
{
    VisibilityInputs inputs;
    inputs.applicationHidden = true;

    return inputs;
}

}  // namespace

TEST_CASE("A moving screen redraws at the frame period, not the display's rate")
{
    // The loop waits out what is left of the period rather than adding a fixed
    // slice on top of it. Presenting already blocks - on the drawable through
    // Metal, on the composition tick through DwmFlush - so a fixed wait would
    // add to that block and halve the rate again on every display at or below
    // 60 Hz.
    FramePacingInputs inputs;
    inputs.lastActivity = 10.0;
    inputs.lastFrameStart = 10.0;
    inputs.now = 10.0;

    const FrameWaitDecision fresh = frameWaitFor(inputs);
    CHECK(fresh.kind == FrameWait::UntilFramePeriod);
    CHECK(fresh.seconds == Catch::Approx(ContentRedrawSeconds));

    // Half the period has already gone into the frame itself; only the rest is
    // waited out.
    inputs.now = 10.0 + ContentRedrawSeconds / 2.0;
    const FrameWaitDecision partway = frameWaitFor(inputs);
    CHECK(partway.seconds > 0.0);
    CHECK(partway.seconds < ContentRedrawSeconds);

    // A frame that already overran the period does not wait at all - which is
    // what leaves a 60 Hz display exactly as it was.
    inputs.now = 10.0 + ContentRedrawSeconds * 2.0;
    const FrameWaitDecision overrun = frameWaitFor(inputs);
    CHECK(overrun.kind == FrameWait::UntilFramePeriod);
    CHECK(overrun.seconds == 0.0);
}

TEST_CASE("A wait that ends early still owes the frame period")
{
    // Every wait here returns on the first event that arrives, and a pointer
    // crossing the window delivers one every few milliseconds. Without a floor
    // on the redraw the loop ran at the event rate exactly when it had decided
    // nothing was happening - 65 frames a second for a picture that never
    // changed.
    FramePacingInputs inputs;
    inputs.lastActivity = 0.0;
    inputs.lastFrameStart = 10.0;
    inputs.now = 10.0;

    const FrameWaitDecision idle = frameWaitFor(inputs);
    REQUIRE(idle.kind == FrameWait::Idle);
    CHECK(idle.redrawFloorSeconds > 0.0);

    inputs.attached = true;
    const FrameWaitDecision watching = frameWaitFor(inputs);
    REQUIRE(watching.kind == FrameWait::WatchAttachedWindow);
    CHECK(watching.redrawFloorSeconds > 0.0);

    // A frame period that has already passed owes nothing, which is the
    // ordinary case: the loop has been asleep.
    inputs.now = 10.0 + 1.0;
    CHECK(frameWaitFor(inputs).redrawFloorSeconds == 0.0);

    // The moving wait needs no floor - it is the pacing itself.
    inputs.lastActivity = 11.0;
    CHECK(frameWaitFor(inputs).redrawFloorSeconds == 0.0);
}

TEST_CASE("A quiet screen falls to the idle tick")
{
    FramePacingInputs inputs;
    inputs.lastActivity = 0.0;
    inputs.lastFrameStart = 0.0;
    inputs.now = IdleAfterSeconds + 0.01;

    const FrameWaitDecision idle = frameWaitFor(inputs);
    CHECK(idle.kind == FrameWait::Idle);
    CHECK(idle.seconds == IdleWaitSeconds);

    // An attached window is watched in short slices instead, so its motion and
    // focus stay fresh - unless the picker owns the screen.
    inputs.attached = true;
    CHECK(frameWaitFor(inputs).kind == FrameWait::WatchAttachedWindow);
    inputs.pickerActive = true;
    CHECK(frameWaitFor(inputs).kind == FrameWait::Idle);
}

TEST_CASE("A readout following the pointer redraws at its own pace")
{
    // The pointer outside the region draws no marker, so the only thing
    // following it is the swatch and its percentages - which look the same at
    // a third of the rate. This is the state a session spends most of its time
    // in, with the user working in the editor beside the scopes.
    FramePacingInputs inputs;
    inputs.lastActivity = 0.0;
    inputs.lastReadoutActivity = 10.0;
    inputs.lastFrameStart = 10.0;
    inputs.now = 10.0;

    const FrameWaitDecision readout = frameWaitFor(inputs);
    CHECK(readout.kind == FrameWait::UntilFramePeriod);
    CHECK(readout.seconds == Catch::Approx(ReadoutRedrawSeconds));
    CHECK(ReadoutRedrawSeconds > ContentRedrawSeconds);

    // A marker moving outranks it: the trace is what carries motion.
    inputs.lastActivity = 10.0;
    CHECK(frameWaitFor(inputs).seconds == Catch::Approx(ContentRedrawSeconds));

    // And a readout that stopped moving lets the loop fall all the way to the
    // idle tick, rather than holding it at the readout cadence for ever.
    inputs.lastActivity = 0.0;
    inputs.now = 10.0 + IdleAfterSeconds + 0.01;
    CHECK(frameWaitFor(inputs).kind == FrameWait::Idle);
}

TEST_CASE("Every way of being out of sight is one")
{
    CHECK_FALSE(outOfSight(InSight));

    // Put away by hand, or the whole session showing nothing.
    for (const auto& set : {&VisibilityInputs::sessionAsleep, &VisibilityInputs::applicationHidden,
                            &VisibilityInputs::iconified, &VisibilityInputs::framebufferEmpty}) {
        VisibilityInputs inputs;
        inputs.*set = true;
        CHECK(outOfSight(inputs));
    }

    VisibilityInputs invisible;
    invisible.windowVisible = false;
    CHECK(outOfSight(invisible));
}

TEST_CASE("A reader of frames holds the stream open however hidden the window")
{
    // The picker paints its own full-screen overlay and the face probe reads
    // frames on its own thread; neither may lose the stream underneath it.
    VisibilityInputs inputs = hidden();
    inputs.sessionAsleep = true;
    inputs.iconified = true;
    inputs.needsFrames = true;
    CHECK_FALSE(outOfSight(inputs));
}

TEST_CASE("The pipeline is suspended only once the window has been gone a while")
{
    // Coming back costs a stream restart, so a flick through the application
    // switcher must not buy one.
    VisibilityGate gate;
    CHECK(gate.update(InSight, false, 0.0) == PipelineAction::Keep);

    CHECK(gate.update(hidden(), false, 1.0) == PipelineAction::Keep);
    CHECK(gate.update(hidden(), false, 1.0 + OutOfSightPauseSeconds - 0.01) == PipelineAction::Keep);
    CHECK(gate.update(hidden(), false, 1.0 + OutOfSightPauseSeconds + 0.01) == PipelineAction::Suspend);

    // Already suspended, the gate asks for nothing more.
    CHECK(gate.update(hidden(), true, 100.0) == PipelineAction::Keep);

    // Back in sight, it resumes once and then leaves it alone.
    CHECK(gate.update(InSight, true, 101.0) == PipelineAction::Resume);
    CHECK(gate.update(InSight, false, 101.1) == PipelineAction::Keep);
}

TEST_CASE("A window that comes back inside the delay is never suspended")
{
    VisibilityGate gate;
    CHECK(gate.update(hidden(), false, 5.0) == PipelineAction::Keep);
    CHECK(gate.update(InSight, false, 5.2) == PipelineAction::Keep);

    // The clock restarts, so the next disappearance waits out the whole delay
    // again rather than inheriting the first one's head start.
    CHECK(gate.update(hidden(), false, 5.3) == PipelineAction::Keep);
    CHECK(gate.update(hidden(), false, 5.3 + OutOfSightPauseSeconds - 0.01) == PipelineAction::Keep);
    CHECK(gate.update(hidden(), false, 5.3 + OutOfSightPauseSeconds + 0.01) == PipelineAction::Suspend);
}

TEST_CASE("Resuming is immediate, however long the window was away")
{
    // Starting the clock at zero matters: the frame clock really does read zero
    // at startup, so a gate that used zero as its in-sight sentinel would miss
    // the first disappearance entirely.
    VisibilityGate gate;
    CHECK(gate.update(hidden(), false, 0.0) == PipelineAction::Keep);
    REQUIRE(gate.update(hidden(), false, 10.0) == PipelineAction::Suspend);

    // No hysteresis on the way back: the scopes are wanted the moment the
    // window is.
    CHECK(gate.update(InSight, true, 10.001) == PipelineAction::Resume);
}

}  // namespace sidescopes
