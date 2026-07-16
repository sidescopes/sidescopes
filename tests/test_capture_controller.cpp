#include <catch2/catch_test_macros.hpp>
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
    CHECK(source.lastFramesPerSecond == 30);
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

}  // namespace sidescopes
