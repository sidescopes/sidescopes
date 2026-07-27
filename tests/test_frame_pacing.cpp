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
    CHECK(fresh.kind == FrameWait::None);
    CHECK(fresh.redrawFloorSeconds == Catch::Approx(ContentRedrawSeconds));

    // Half the period has already gone into the frame itself; only the rest is
    // waited out.
    inputs.now = 10.0 + ContentRedrawSeconds / 2.0;
    const FrameWaitDecision partway = frameWaitFor(inputs);
    CHECK(partway.redrawFloorSeconds > 0.0);
    CHECK(partway.redrawFloorSeconds < ContentRedrawSeconds);

    // A frame that already overran the period does not wait at all - which is
    // what leaves a 60 Hz display exactly as it was.
    inputs.now = 10.0 + ContentRedrawSeconds * 2.0;
    const FrameWaitDecision overrun = frameWaitFor(inputs);
    CHECK(overrun.kind == FrameWait::None);
    CHECK(overrun.redrawFloorSeconds == 0.0);
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

    // The moving case blocks on nothing at all, so the floor is its whole
    // wait rather than a backstop under one.
    inputs.now = 10.0;
    inputs.lastActivity = 10.0;
    const FrameWaitDecision movingWait = frameWaitFor(inputs);
    CHECK(movingWait.kind == FrameWait::None);
    CHECK(movingWait.redrawFloorSeconds > 0.0);
}

TEST_CASE("A quiet screen falls to the idle tick")
{
    FramePacingInputs inputs;
    inputs.lastActivity = 0.0;
    inputs.lastFrameStart = 0.0;
    inputs.now = IdleAfterSeconds + 0.01;

    CHECK(frameWaitFor(inputs).kind == FrameWait::Idle);

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
    CHECK(readout.kind == FrameWait::None);
    CHECK(readout.redrawFloorSeconds == Catch::Approx(ReadoutRedrawSeconds));
    CHECK(ReadoutRedrawSeconds > ContentRedrawSeconds);

    // A marker moving outranks it: the trace is what carries motion.
    inputs.lastActivity = 10.0;
    CHECK(frameWaitFor(inputs).redrawFloorSeconds == Catch::Approx(ContentRedrawSeconds));

    // And a readout that stopped moving lets the loop fall all the way to the
    // idle tick, rather than holding it at the readout cadence for ever.
    inputs.lastActivity = 0.0;
    inputs.now = 10.0 + IdleAfterSeconds + 0.01;
    CHECK(frameWaitFor(inputs).kind == FrameWait::Idle);
}

namespace {

// The application quiet: nothing has happened for long enough that the last
// frame drawn is still the right picture.
RedrawInputs quiet()
{
    RedrawInputs inputs;
    inputs.now = 100.0;
    inputs.lastActivity = 50.0;
    inputs.lastReadoutActivity = 50.0;
    inputs.lastInputEvent = 50.0;
    inputs.lastDrawn = 50.0;

    return inputs;
}

}  // namespace

TEST_CASE("A quiet application draws nothing at all")
{
    // Not a slower cadence - none. The driver holds its render arena for as
    // long as frames keep arriving, so two a second cost as much as sixty.
    CHECK_FALSE(frameWorthDrawing(quiet()));

    // Every reason a picture can differ from the one on screen brings the
    // frame back.
    for (const auto& set : {&RedrawInputs::outputPending, &RedrawInputs::textInputActive, &RedrawInputs::overlayActive,
                            &RedrawInputs::framebufferChanged, &RedrawInputs::statusChanged}) {
        RedrawInputs inputs = quiet();
        inputs.*set = true;
        CHECK(frameWorthDrawing(inputs));
    }
}

TEST_CASE("An interaction owes frames after its last event")
{
    // Hover highlights, tooltip delays and text cursors all advance on drawn
    // frames, so the frames that finish a gesture have to outlast it. A
    // pointer that settles over a tool and stops sending events is the case:
    // its tooltip is still due.
    RedrawInputs inputs = quiet();
    inputs.lastInputEvent = inputs.now - InputSettleSeconds + 0.01;
    CHECK(frameWorthDrawing(inputs));

    inputs.lastInputEvent = inputs.now - InputSettleSeconds - 0.01;
    CHECK_FALSE(frameWorthDrawing(inputs));
    CHECK(InputSettleSeconds > IdleAfterSeconds);

    // The other clocks the pacing keeps hold frames the same way.
    for (const auto& clock :
         {&RedrawInputs::lastActivity, &RedrawInputs::lastReadoutActivity, &RedrawInputs::lastPointerMove}) {
        RedrawInputs recent = quiet();
        recent.*clock = recent.now - IdleAfterSeconds + 0.01;
        CHECK(frameWorthDrawing(recent));
    }
}

TEST_CASE("A moving pointer is a reason to draw all by itself")
{
    // The case nothing else here covers, and the ordinary one beside an editor:
    // the pointer crosses a still photograph in another application's window.
    // Not one pixel of the screen changes, so no pass is published; this window
    // is not frontmost, so no event reaches it; and every clock says the picture
    // is finished. But a marker and the colour readout are readings of the point
    // under the pointer, and the point has moved.
    RedrawInputs inputs = quiet();
    REQUIRE_FALSE(frameWorthDrawing(inputs));

    inputs.lastPointerMove = inputs.now;
    CHECK(frameWorthDrawing(inputs));

    // It owes frames for a moment after the pointer stops, because a marker
    // takes its colour on its own interval and cannot set off before the next
    // one lands.
    inputs.lastPointerMove = inputs.now - IdleAfterSeconds + 0.01;
    CHECK(frameWorthDrawing(inputs));

    // And no longer than that: a pointer parked over a picture nobody is
    // editing leaves the application drawing nothing at all.
    inputs.lastPointerMove = inputs.now - IdleAfterSeconds - 0.01;
    CHECK_FALSE(frameWorthDrawing(inputs));
}

TEST_CASE("A moving pointer paces the loop like the readout it carries")
{
    // The same argument on the waiting side. Without this the loop blocks for
    // the idle tick, and a pointer moved over an unchanging screen is noticed
    // half a second later - which is most of what a reading taken by hovering
    // felt like it cost.
    FramePacingInputs inputs;
    inputs.lastActivity = 0.0;
    inputs.lastReadoutActivity = 0.0;
    inputs.lastFrameStart = 10.0;
    inputs.now = 10.0;
    REQUIRE(frameWaitFor(inputs).kind == FrameWait::Idle);

    inputs.lastPointerMove = 10.0;
    const FrameWaitDecision pointing = frameWaitFor(inputs);
    CHECK(pointing.kind == FrameWait::None);
    // A swatch and a marker, not a moving trace: the readout's own cadence.
    CHECK(pointing.redrawFloorSeconds == Catch::Approx(ReadoutRedrawSeconds));

    // A trace actually moving still outranks it.
    inputs.lastActivity = 10.0;
    CHECK(frameWaitFor(inputs).redrawFloorSeconds == Catch::Approx(ContentRedrawSeconds));

    // A pointer that stopped lets the loop fall all the way back to the idle
    // tick rather than holding it at the readout cadence for ever.
    inputs.lastActivity = 0.0;
    inputs.now = 10.0 + IdleAfterSeconds + 0.01;
    CHECK(frameWaitFor(inputs).kind == FrameWait::Idle);
}

TEST_CASE("A region under the hand is followed, not paced")
{
    // The regression this exists to stop: the region border and the picker's
    // rubber band are repositioned from the frame loop, so whatever paces the
    // loop paces them. At a 50 ms frame period a border flicked at 1600 points
    // a second trailed the pointer by 83 ms on average and 158 at worst -
    // twenty-odd points of stickiness under a hand that is drawing roughly and
    // quickly, which is how a region is drawn.
    FramePacingInputs inputs;
    inputs.lastActivity = 10.0;
    inputs.lastFrameStart = 10.0;
    inputs.now = 10.0;
    REQUIRE(frameWaitFor(inputs).redrawFloorSeconds > 0.0);

    inputs.regionInteracting = true;
    const FrameWaitDecision following = frameWaitFor(inputs);
    CHECK(following.kind == FrameWait::FollowInteraction);
    // No floor at all: the wait ends on the pointer event, and the loop applies
    // it and moves the border before taking the next one.
    CHECK(following.redrawFloorSeconds == 0.0);

    // It outranks every other reason to wait, including the two that block.
    for (const auto& set : {&FramePacingInputs::attached, &FramePacingInputs::pickerActive}) {
        FramePacingInputs other = inputs;
        other.lastActivity = 0.0;
        other.now = 10.0 + IdleAfterSeconds + 0.01;
        other.*set = true;
        CHECK(frameWaitFor(other).kind == FrameWait::FollowInteraction);
    }

    // And a hand that pauses mid-gesture is not left waiting on the idle tick.
    CHECK(InteractionWaitSeconds <= ContentRedrawSeconds);
}

TEST_CASE("A followed region still redraws at the frame period")
{
    // The wait is what holds the frame period everywhere else, and a followed
    // region has none - so without this the window itself would redraw once per
    // pointer event, a hundred times a second, for scope images that arrive
    // fifteen. What the hand is watching is the border, and no frame of this
    // window draws it.
    RedrawInputs inputs = quiet();
    inputs.lastPointerMove = inputs.now;
    inputs.lastDrawn = inputs.now - ContentRedrawSeconds / 2.0;
    REQUIRE(frameWorthDrawing(inputs));

    inputs.regionInteracting = true;
    CHECK_FALSE(frameWorthDrawing(inputs));

    // A period is a period, not a mute: the frame comes back when it is due.
    inputs.lastDrawn = inputs.now - ContentRedrawSeconds - 0.001;
    CHECK(frameWorthDrawing(inputs));

    // It gives way to nothing else, either - a picker overlay up over a region
    // being drawn is still worth exactly one frame a period.
    RedrawInputs overlay = quiet();
    overlay.overlayActive = true;
    overlay.regionInteracting = true;
    overlay.lastDrawn = overlay.now;
    CHECK_FALSE(frameWorthDrawing(overlay));
}

TEST_CASE("Something that leaves the screen by itself is taken away")
{
    // A status message, the attach notice and an intensity readout all expire
    // on a clock rather than on an event. Nothing else takes them off screen,
    // so the loop owes exactly one frame at the moment they do - and stops
    // again once that frame has drawn the row without them.
    RedrawInputs inputs = quiet();
    inputs.redrawDue = inputs.now + 1.0;
    CHECK_FALSE(frameWorthDrawing(inputs));

    inputs.now = inputs.redrawDue + 0.01;
    CHECK(frameWorthDrawing(inputs));

    inputs.lastDrawn = inputs.now;
    CHECK_FALSE(frameWorthDrawing(inputs));

    // Nothing timed has ever been shown, which is the ordinary case and must
    // not read as one that expired at the beginning of time.
    RedrawInputs never = quiet();
    never.redrawDue = 0.0;
    CHECK_FALSE(frameWorthDrawing(never));
}

TEST_CASE("Every reason to stop capturing is one")
{
    CHECK_FALSE(nothingNeedsFrames(InSight));

    // Put away by hand, the whole session showing nothing, or nothing selected
    // to read: the window in front of the user with no region is as free of
    // readers as a hidden one, since the colour under the pointer has an
    // off-stream sample of its own.
    for (const auto& set :
         {&VisibilityInputs::sessionAsleep, &VisibilityInputs::applicationHidden, &VisibilityInputs::iconified,
          &VisibilityInputs::framebufferEmpty, &VisibilityInputs::nothingSelected}) {
        VisibilityInputs inputs;
        inputs.*set = true;
        CHECK(nothingNeedsFrames(inputs));
    }

    VisibilityInputs invisible;
    invisible.windowVisible = false;
    CHECK(nothingNeedsFrames(invisible));
}

TEST_CASE("A selected region holds the stream open on a visible window")
{
    // The everyday state: the window in sight with something to read. Nothing
    // here may stop the capture.
    VisibilityInputs inputs = InSight;
    inputs.nothingSelected = false;
    CHECK_FALSE(nothingNeedsFrames(inputs));

    // And a pick opened from the empty state holds it open even before a
    // region exists, because the picker reads frames itself.
    VisibilityInputs picking;
    picking.nothingSelected = true;
    picking.needsFrames = true;
    CHECK_FALSE(nothingNeedsFrames(picking));
}

TEST_CASE("A reader of frames holds the stream open however hidden the window")
{
    // The picker paints its own full-screen overlay and the face probe reads
    // frames on its own thread; neither may lose the stream underneath it.
    VisibilityInputs inputs = hidden();
    inputs.sessionAsleep = true;
    inputs.iconified = true;
    inputs.needsFrames = true;
    CHECK_FALSE(nothingNeedsFrames(inputs));
}

TEST_CASE("The pipeline is suspended only once the window has been gone a while")
{
    // Coming back costs a stream restart, so a flick through the application
    // switcher must not buy one.
    VisibilityGate gate;
    CHECK(gate.update(InSight, false, 0.0) == PipelineAction::Keep);

    CHECK(gate.update(hidden(), false, 1.0) == PipelineAction::Keep);
    CHECK(gate.update(hidden(), false, 1.0 + CapturePauseSeconds - 0.01) == PipelineAction::Keep);
    CHECK(gate.update(hidden(), false, 1.0 + CapturePauseSeconds + 0.01) == PipelineAction::Suspend);

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
    CHECK(gate.update(hidden(), false, 5.3 + CapturePauseSeconds - 0.01) == PipelineAction::Keep);
    CHECK(gate.update(hidden(), false, 5.3 + CapturePauseSeconds + 0.01) == PipelineAction::Suspend);
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

TEST_CASE("Every clock the loop keeps reaches the decision it belongs to")
{
    FrameClocks clocks;
    clocks.noteActivity(1.0);
    clocks.noteReadoutActivity(2.0);
    clocks.notePointerMove(3.0);
    clocks.notePumpReturned(4.0);
    clocks.noteFrameBegun(5.0);

    const FramePacingInputs pacing = clocks.pacingInputs(9.0, true, false, true);
    CHECK(pacing.now == 9.0);
    CHECK(pacing.lastActivity == 1.0);
    CHECK(pacing.lastReadoutActivity == 2.0);
    CHECK(pacing.lastPointerMove == 3.0);
    CHECK(pacing.lastFrameStart == 4.0);
    CHECK(pacing.attached);
    CHECK_FALSE(pacing.pickerActive);
    CHECK(pacing.regionInteracting);

    RedrawSignals signals;
    signals.lastInputEvent = 6.0;
    signals.redrawDue = 7.0;
    signals.textInputActive = true;
    signals.overlayActive = true;
    signals.regionInteracting = true;
    const RedrawInputs redraw = clocks.redrawInputs(signals, 9.0);
    CHECK(redraw.now == 9.0);
    CHECK(redraw.lastActivity == 1.0);
    CHECK(redraw.lastReadoutActivity == 2.0);
    CHECK(redraw.lastPointerMove == 3.0);
    CHECK(redraw.lastInputEvent == 6.0);
    CHECK(redraw.lastDrawn == 5.0);
    CHECK(redraw.redrawDue == 7.0);
    CHECK(redraw.textInputActive);
    CHECK(redraw.overlayActive);
    CHECK(redraw.regionInteracting);
}

TEST_CASE("A published pass is pending until a frame begins")
{
    FrameClocks clocks;
    const RedrawSignals signals;
    CHECK_FALSE(clocks.redrawInputs(signals, 0.0).outputPending);

    clocks.noteOutputPublished();
    CHECK(clocks.redrawInputs(signals, 0.0).outputPending);

    // Cleared where the frame begins, not where it ends: a pass published
    // while that frame is being built is owed a frame of its own.
    clocks.noteFrameBegun(1.0);
    CHECK_FALSE(clocks.redrawInputs(signals, 1.0).outputPending);
}

TEST_CASE("A frame is owed until one has been drawn at this size and status")
{
    FrameClocks clocks;
    RedrawSignals signals;
    signals.framebufferWidth = 800;
    signals.framebufferHeight = 600;
    signals.captureStatus = "capturing";

    // Nothing has been drawn at all, so the first frame is owed on both counts.
    CHECK(clocks.redrawInputs(signals, 0.0).framebufferChanged);
    CHECK(clocks.redrawInputs(signals, 0.0).statusChanged);

    clocks.noteFrameShown(800, 600, "capturing");
    CHECK_FALSE(clocks.redrawInputs(signals, 0.0).framebufferChanged);
    CHECK_FALSE(clocks.redrawInputs(signals, 0.0).statusChanged);

    signals.framebufferHeight = 601;
    CHECK(clocks.redrawInputs(signals, 0.0).framebufferChanged);

    signals.framebufferHeight = 600;
    signals.captureStatus = "paused - no region selected";
    CHECK(clocks.redrawInputs(signals, 0.0).statusChanged);
}

TEST_CASE("Following a hand waits no longer than the frame it is holding up")
{
    FrameClocks clocks;
    clocks.noteFrameBegun(10.0);

    // Most of the period is still to run, so that is the whole of the wait.
    CHECK(clocks.interactionWait(10.0 + ContentRedrawSeconds * 0.25) == Catch::Approx(ContentRedrawSeconds * 0.75));

    // The frame is already due: there is nothing left to wake for, and the
    // ceiling is the honest wait.
    CHECK(clocks.interactionWait(10.0 + ContentRedrawSeconds) == InteractionWaitSeconds);
    CHECK(clocks.interactionWait(20.0) == InteractionWaitSeconds);

    // And it is never longer than one slice, whatever the clocks say.
    CHECK(clocks.interactionWait(0.0) == InteractionWaitSeconds);
}

}  // namespace sidescopes
