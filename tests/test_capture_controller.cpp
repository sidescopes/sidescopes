#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <string>

#include "app/capture_controller.h"
#include "core/frame_mailbox.h"
#include "fake_capture.h"

namespace sidescopes {

using test::FakeCaptureSource;
using test::makeTarget;

TEST_CASE("A denied permission keeps the controller from touching the source")
{
    FakeCaptureSource source;
    source.permission = CapturePermission::Denied;
    source.targets = {makeTarget(1, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);

    CHECK_FALSE(controller.requestPermission());
    CHECK_FALSE(controller.permissionGranted());
    CHECK_FALSE(controller.start());
    // start() must not have listed targets or started a stream.
    CHECK(source.startCount == 0);
    CHECK(source.stopCount == 0);
    CHECK(controller.status().find("permission missing") != std::string::npos);
}

TEST_CASE("start captures the first target when no display is desired")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "primary"), makeTarget(9, "secondary")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());

    REQUIRE(controller.start());
    CHECK(source.startCount == 1);
    CHECK(source.stopCount == 0);  // the first start has nothing to stop
    CHECK(source.lastStartedDisplay == 7);
    // Fifteen a second, chosen from measurement rather than from the display:
    // it costs 32% less processor time than thirty over changing content, and
    // a whole-display region never managed more than fourteen passes a second
    // anyway. What it buys with is the step between updates - see the constant
    // in capture_controller.cpp, which carries the numbers.
    CHECK(source.lastFramesPerSecond == 15);
    CHECK(controller.capturedDisplay() == 7);
    CHECK(controller.desiredDisplay() == 7);
    CHECK_FALSE(controller.dead());
    CHECK(controller.status() == "capturing primary");
}

TEST_CASE("start captures the display the user requested")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "primary"), makeTarget(9, "secondary")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    controller.requestDisplay(9);

    REQUIRE(controller.start());
    CHECK(source.lastStartedDisplay == 9);
    CHECK(controller.capturedDisplay() == 9);
    CHECK(controller.desiredDisplay() == 9);
    CHECK(controller.status() == "capturing secondary");
}

TEST_CASE("A requested display that is gone pauses on the disconnect banner")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "primary")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    controller.requestDisplay(9);

    CHECK_FALSE(controller.start());
    CHECK(source.startCount == 0);
    CHECK(controller.capturedDisplay() == 0);
    CHECK(controller.status() == "display disconnected - scopes resume when it returns");
}

TEST_CASE("The source's status callback marks the stream dead and posts the message")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "primary")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    CHECK_FALSE(controller.dead());

    // A backend thread reports the stream stopped.
    source.fireStatus("capture stream stopped");
    CHECK(controller.dead());
    CHECK(controller.status() == "capture stream stopped");
}

TEST_CASE("service restarts a dead stream and backs off two seconds when it fails")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "primary")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());

    // The stream dies; the next restart is scripted to fail.
    source.fireStatus("interrupted");
    REQUIRE(controller.dead());
    source.startSucceeds = false;

    // A callback death carries no backoff, so the first service past t=0
    // retries at once; the running-but-dead stream is torn down first.
    controller.service(1.0);
    CHECK(source.startCount == 2);
    CHECK(source.stopCount == 1);
    CHECK(controller.dead());

    // Inside the two-second backoff, service leaves the source alone.
    controller.service(2.5);
    CHECK(source.startCount == 2);

    // Past the deadline it retries; this time the restart succeeds.
    source.startSucceeds = true;
    controller.service(3.5);
    CHECK(source.startCount == 3);
    CHECK_FALSE(controller.dead());
    CHECK(controller.capturedDisplay() == 7);
}

TEST_CASE("A stale mark restarts the stream after a one-second backoff")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "primary")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());

    controller.markStale();

    // Consuming the stale mark marks the stream dead and sets a one-second
    // backoff, so the same tick does not restart yet.
    controller.service(10.0);
    CHECK(controller.dead());
    CHECK(source.startCount == 1);
    CHECK(source.stopCount == 0);

    // Still inside the backoff: nothing happens.
    controller.service(10.5);
    CHECK(source.startCount == 1);

    // Past t + 1 s the stream is torn down once and restarted.
    controller.service(11.5);
    CHECK(source.stopCount == 1);
    CHECK(source.startCount == 2);
    CHECK_FALSE(controller.dead());
}

TEST_CASE("a stream that stays unreachable is retried less and less often")
{
    // What a locked screen or a sleeping display looks like from here: every
    // restart fails, for hours. A fixed two-second retry would run seven
    // hundred times an hour for a window nobody can see.
    FakeCaptureSource source;
    source.targets = {makeTarget(1, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    source.fireStatus("capture stopped");
    REQUIRE(controller.dead());
    source.startSucceeds = false;

    // The first retry is as prompt as it ever was, then each failure doubles
    // the wait: 2, 4, 8, 16, then the half-minute ceiling. Each wait is timed
    // from the attempt that failed, not from a round number, so the clock walks
    // a moment past each deadline.
    double clock = 0.0;
    int attempts = source.startCount;
    for (const double wait : {2.0, 4.0, 8.0, 16.0, 30.0, 30.0}) {
        clock += 0.001;
        controller.service(clock);
        CHECK(source.startCount == attempts + 1);
        attempts = source.startCount;

        // Still inside the wait: nothing happens.
        controller.service(clock + wait - 0.01);
        CHECK(source.startCount == attempts);
        clock += wait;
    }

    // Six failures spread over ninety seconds. A flat two-second retry would
    // have made forty-five attempts across the same span.
    CHECK(source.startCount - 1 == 6);
    CHECK(clock > 89.0);
    CHECK(clock < 91.0);
}

TEST_CASE("a wake earns a prompt retry however long the backoff had grown")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(1, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    source.fireStatus("capture stopped");
    source.startSucceeds = false;

    double clock = 0.0;
    for (const double wait : {2.0, 4.0, 8.0, 16.0}) {
        clock += 0.001;
        controller.service(clock);
        clock += wait;
    }
    const int beforeWake = source.startCount;

    // The screen comes back. The conditions that were failing have changed, so
    // the next attempt must not wait out the backoff those failures earned -
    // only the wake's own one-second grace.
    source.startSucceeds = true;
    controller.markStale();
    controller.service(clock);
    CHECK(source.startCount == beforeWake);
    controller.service(clock + 1.5);
    CHECK(source.startCount == beforeWake + 1);
    CHECK_FALSE(controller.dead());
}

TEST_CASE("a stream that comes back forgets the failures before it")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(1, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    source.fireStatus("capture stopped");
    source.startSucceeds = false;

    double clock = 0.0;
    for (const double wait : {2.0, 4.0, 8.0}) {
        clock += 0.001;
        controller.service(clock);
        clock += wait;
    }
    source.startSucceeds = true;
    clock += 0.001;
    controller.service(clock);
    REQUIRE_FALSE(controller.dead());

    // A later failure starts the backoff over at two seconds rather than
    // resuming where the old run left off.
    source.startSucceeds = false;
    source.fireStatus("capture stopped");
    const int attempts = source.startCount;
    clock += 0.001;
    controller.service(clock);
    CHECK(source.startCount == attempts + 1);
    controller.service(clock + 1.9);
    CHECK(source.startCount == attempts + 1);
    controller.service(clock + 2.1);
    CHECK(source.startCount == attempts + 2);
}

TEST_CASE("suspend lets go of the frames as well as the stream")
{
    // A stopped stream keeping a display's worth of pixels warm for deliveries
    // that are not coming is the largest single allocation this application
    // makes, so the pause frees the mailbox rather than only stopping the
    // producer that fills it.
    FakeCaptureSource source;
    source.targets = {makeTarget(4, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());

    FrameBuffer filled;
    filled.width = 64;
    filled.height = 32;
    filled.strideBytes = 64 * 4;
    filled.data.assign(static_cast<std::size_t>(64) * 32 * 4, 0);
    FrameBuffer recycled = mailbox.publish(std::move(filled));
    recycled.data.assign(static_cast<std::size_t>(64) * 32 * 4, 0);
    mailbox.returnStorage(std::move(recycled));

    controller.suspend("paused");

    // Nothing pending: the frame nobody took is gone rather than waiting.
    CHECK_FALSE(mailbox.takeLatest(std::chrono::milliseconds(0)).has_value());
    // And nothing kept for reuse: the next publish is handed empty storage.
    FrameBuffer next;
    next.width = 8;
    next.height = 8;
    next.strideBytes = 8 * 4;
    next.data.assign(static_cast<std::size_t>(8) * 8 * 4, 0);
    const FrameBuffer afterPause = mailbox.publish(std::move(next));
    CHECK(afterPause.data.empty());
}

TEST_CASE("suspend stops the stream and service leaves it stopped")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(4, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());

    // A pause carries its reason to the status line: the two callers stop for
    // different reasons and the settings window says which.
    controller.suspend("paused - the window is out of sight");
    CHECK(controller.suspended());
    CHECK(source.stopCount == 1);
    // A stream stopped on purpose has not died, or service would revive it.
    CHECK_FALSE(controller.dead());
    CHECK(controller.status().find("out of sight") != std::string::npos);

    // However long the frame loop runs, a suspended controller starts nothing.
    controller.service(100.0);
    controller.service(200.0);
    CHECK(source.startCount == 1);
    CHECK(source.stopCount == 1);

    controller.resume();
    CHECK_FALSE(controller.suspended());
    CHECK(source.startCount == 2);
    CHECK(controller.capturedDisplay() == 4);
    CHECK(controller.status() == "capturing main");
}

TEST_CASE("suspending twice or resuming unsuspended does nothing")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(4, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());

    // The frame loop asks every frame, so both calls must be idempotent.
    controller.resume();
    CHECK(source.startCount == 1);

    controller.suspend("paused");
    controller.suspend("paused");
    CHECK(source.stopCount == 1);

    controller.resume();
    controller.resume();
    CHECK(source.startCount == 2);
}

TEST_CASE("a wake during a suspension is answered by the resume itself")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(4, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    controller.suspend("paused");

    // The display slept and woke while the window was away. The resume starts a
    // fresh stream, which is exactly what the stale mark was asking for, so it
    // must not buy a second restart afterwards.
    controller.markStale();
    controller.resume();
    CHECK(source.startCount == 2);
    CHECK_FALSE(controller.dead());

    controller.service(500.0);
    CHECK(source.startCount == 2);
    CHECK_FALSE(controller.dead());
}

TEST_CASE("a stream that dies while suspended is not restarted until it returns")
{
    FakeCaptureSource source;
    source.targets = {makeTarget(4, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    controller.suspend("paused");

    // The backend reports the stop from its own thread after suspend returned.
    source.fireStatus("capture stopped");
    controller.service(600.0);
    CHECK(source.startCount == 1);

    controller.resume();
    CHECK(source.startCount == 2);
    CHECK_FALSE(controller.dead());
}

TEST_CASE("Narrowing reaches the stream only while it is delivering")
{
    // A stream that is not running, or is suspended, has nothing to narrow - and
    // a restart begins on the whole display, so the next decision re-applies
    // whatever is wanted. Forwarding regardless would ask a dead stream to
    // reconfigure.
    FakeCaptureSource source;
    source.targets = {makeTarget(1, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);

    const IntRect canvas{100, 100, 800, 600};

    // Before start: ignored.
    controller.narrowTo(canvas);
    CHECK(source.narrowings.empty());

    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    controller.narrowTo(canvas);
    REQUIRE(source.narrowings.size() == 1);
    REQUIRE(source.narrowings.back().has_value());
    CHECK(*source.narrowings.back() == canvas);

    // Asking for the whole display is a request in its own right, not silence.
    controller.narrowTo(std::nullopt);
    REQUIRE(source.narrowings.size() == 2);
    CHECK_FALSE(source.narrowings.back().has_value());

    // Suspended: ignored, because the stream is not delivering.
    controller.suspend("paused");
    controller.narrowTo(canvas);
    CHECK(source.narrowings.size() == 2);

    // And honoured again once it is.
    controller.resume();
    controller.narrowTo(canvas);
    CHECK(source.narrowings.size() == 3);
}

}  // namespace sidescopes
