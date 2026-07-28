#include <algorithm>
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
    // in capture_controller.h, which carries the numbers.
    CHECK(source.lastFramesPerSecond == DefaultCaptureFramesPerSecond);
    CHECK(DefaultCaptureFramesPerSecond == 15);
    CHECK(controller.capturedDisplay() == 7);
    CHECK(controller.desiredDisplay() == 7);
    CHECK_FALSE(controller.dead());
    CHECK(controller.status() == "capturing primary");
}

TEST_CASE("A new rate replaces the stream that carries the old one")
{
    // The rate is fixed when a stream is created, so the only way to change it
    // is to make another. A quality level is chosen once and rarely, which is
    // what makes a restart the right price.
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "primary")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    CHECK(source.lastFramesPerSecond == DefaultCaptureFramesPerSecond);

    controller.setFrameRate(10);
    CHECK(source.startCount == 2);
    CHECK(source.stopCount == 1);
    CHECK(source.lastFramesPerSecond == 10);
    CHECK(controller.capturedDisplay() == 7);

    // The same rate again is not a reason to interrupt the stream.
    controller.setFrameRate(10);
    CHECK(source.startCount == 2);
}

TEST_CASE("A rate chosen while nothing is running waits for the next start")
{
    // A suspended stream is stopped on purpose, and restarting it here would
    // undo the pause the visibility gate asked for. The next start reads the
    // rate anyway.
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "primary")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    controller.suspend("out of sight");

    controller.setFrameRate(20);
    CHECK(source.startCount == 1);
    CHECK(controller.suspended());

    controller.resume();
    CHECK(source.startCount == 2);
    CHECK(source.lastFramesPerSecond == 20);
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
    // the wait: 2, 4, then the ceiling. Each wait is timed from the attempt
    // that failed, not from a round number, so the clock walks a moment past
    // each deadline.
    //
    // The ceiling is a handful of seconds because it is also the longest a
    // user can be left on a page that says the capture is coming back - the
    // half-minute it used to be was long enough to read as never.
    double clock = 0.0;
    int attempts = source.startCount;
    for (const double wait : {2.0, 4.0, 5.0, 5.0, 5.0, 5.0}) {
        clock += 0.001;
        controller.service(clock);
        CHECK(source.startCount == attempts + 1);
        attempts = source.startCount;

        // Still inside the wait: nothing happens.
        controller.service(clock + wait - 0.01);
        CHECK(source.startCount == attempts);
        clock += wait;
    }

    // Six failures spread over twenty-six seconds. A flat two-second retry
    // would have made thirteen attempts across the same span.
    CHECK(source.startCount - 1 == 6);
    CHECK(clock > 25.0);
    CHECK(clock < 27.0);
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
    for (const double wait : {2.0, 4.0, 5.0, 5.0}) {
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
    for (const double wait : {2.0, 4.0, 5.0}) {
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
    controller.suspend("paused - no region selected");

    // The backend reports the stop from its own thread after suspend returned,
    // which is the shape the application was stranded by: the report says the
    // stream is gone, and a paused controller will not build another. It must
    // therefore not read as a failure either, or the application stands on a
    // page promising a reconnection nothing is attempting - and with no region
    // selected nothing will resume the pipeline until the user draws one.
    source.fireStatus("capture stopped: no displays or windows to capture");
    controller.service(600.0);
    CHECK(source.startCount == 1);
    CHECK_FALSE(controller.dead());
    // And the line the user reads says why the pipeline is paused, not what
    // the stream said on its way out.
    CHECK(controller.status() == "paused - no region selected");

    controller.resume();
    CHECK(source.startCount == 2);
    CHECK_FALSE(controller.dead());
}

TEST_CASE("capture wanted and absent is retried for as long as it is absent")
{
    // THE INVARIANT THE RECOVERY RESTS ON, stated without naming a cause. The
    // stream is gone; nothing tells the controller why, nothing announces that
    // conditions changed, and no region, frame, wake or window event ever
    // arrives. Attempts must keep coming anyway, and no wait between them may
    // exceed the ceiling - which is what makes recovery a property of being
    // broken rather than of the application recognising how it broke.
    FakeCaptureSource source;
    source.targets = {makeTarget(1, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    source.fireStatus("capture stopped");
    source.startSucceeds = false;

    // Ten minutes of frame loop at the idle tick, and not one signal.
    constexpr double Tick = 0.05;
    constexpr int Ticks = 600 * 20;
    int attempts = 0;
    double lastAttempt = 0.0;
    double longestGap = 0.0;
    for (int tick = 1; tick <= Ticks; ++tick) {
        const double now = static_cast<double>(tick) * Tick;
        const int before = source.startCount;
        controller.service(now);
        if (source.startCount != before) {
            longestGap = std::max(longestGap, now - lastAttempt);
            lastAttempt = now;
            ++attempts;
        }
    }

    // Still trying, and still saying so.
    CHECK(controller.dead());
    // Never longer than the ceiling plus the tick that lands past it: a user
    // watching the reconnection notice waits seconds, not minutes. The slack
    // is one more tick, for the clock this walks in fiftieths of a second.
    CHECK(longestGap < 5.0 + (2.0 * Tick));
    // And not a handful of attempts that quietly gave up.
    CHECK(attempts > 70);

    // The world allows it again, with nothing to announce that it has.
    source.startSucceeds = true;
    controller.service(static_cast<double>(Ticks) * Tick + 8.1);
    CHECK_FALSE(controller.dead());
    CHECK(controller.status() == "capturing main");
}

TEST_CASE("a healthy pause stays paused, stays quiet and stays honest")
{
    // The other half of the invariant, and the one that protects the measured
    // idle cost: a pipeline paused because nothing is asking for frames must
    // make no attempts at all, however long it is left. The empty state is the
    // one this application sits in most, so a retry loop leaking into it would
    // cost more than the pause ever saved.
    FakeCaptureSource source;
    source.targets = {makeTarget(4, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    controller.suspend("paused - no region selected");

    for (int tick = 1; tick <= 600 * 20; ++tick) {
        controller.service(static_cast<double>(tick) * 0.05);
    }
    CHECK(source.startCount == 1);
    CHECK(source.stopCount == 1);
    CHECK_FALSE(controller.dead());
    CHECK(controller.status() == "paused - no region selected");
}

TEST_CASE("a resume that cannot reach a display keeps trying until it can")
{
    // Waking to displays that have not all come back, or to the one being
    // captured gone for good. Nothing was running to report the failure, so
    // the resume's own start is the only thing that knows about it - and the
    // controller has to keep going from there without being told.
    FakeCaptureSource source;
    source.targets = {makeTarget(4, "main")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    REQUIRE(controller.start());
    controller.suspend("paused - the window is out of sight");

    source.targets.clear();
    controller.resume();
    CHECK_FALSE(controller.suspended());
    CHECK(controller.dead());
    CHECK(controller.status() == "no display available - scopes resume when one returns");

    const int afterResume = source.startCount;
    controller.service(3.0);
    CHECK(source.startCount == afterResume);  // listed, found nothing, no stream

    source.targets = {makeTarget(4, "main")};
    controller.service(20.0);
    CHECK(source.startCount == afterResume + 1);
    CHECK_FALSE(controller.dead());
}

TEST_CASE("a display chosen while paused is taken up by the resume, not behind it")
{
    // The window is dragged, or its monitor unplugged and the window moved for
    // it, while the pipeline is paused. Capture follows the window, so a new
    // display is asked for - but starting one here would leave a live stream
    // behind a controller that is servicing nothing, delivering frames nobody
    // asked for until something happens to notice.
    FakeCaptureSource source;
    source.targets = {makeTarget(7, "external"), makeTarget(4, "built-in")};
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());
    controller.requestDisplay(7);
    REQUIRE(controller.start());
    controller.suspend("paused - no region selected");

    controller.requestDisplay(4);
    CHECK_FALSE(controller.start());
    CHECK(source.startCount == 1);
    CHECK(controller.suspended());
    CHECK_FALSE(controller.dead());

    // The choice was not lost, only deferred to the moment frames are wanted.
    controller.resume();
    CHECK(source.startCount == 2);
    CHECK(source.lastStartedDisplay == 4);
    CHECK(controller.capturedDisplay() == 4);
}

TEST_CASE("a first start that fails is retried like any other")
{
    // Launching while the display is asleep, or into a session that has none
    // yet. There is no earlier stream to have died, so nothing reports this -
    // and a controller that treated a failed first start as quietly stopped
    // would never capture anything for the rest of the run.
    FakeCaptureSource source;
    FrameMailbox mailbox;
    CaptureController controller(source, mailbox);
    REQUIRE(controller.requestPermission());

    CHECK_FALSE(controller.start());
    CHECK(controller.dead());
    CHECK(controller.status() == "no display available - scopes resume when one returns");

    source.targets = {makeTarget(4, "main")};
    controller.service(1.0);
    CHECK(source.startCount == 1);
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
