#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "app/capture_controller.h"
#include "app/region_picker.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "fake_capture.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"
#include "test_frame.h"

// The picker reaches the platform picker overlay, the desktop services, and the
// wall clock directly, but the test binary links no windowing library and no
// desktop backend. It provides its own stubs: the overlay accepts an open and
// reports nothing, the desktop services answer empty, and face detection is off
// so no background scan thread is ever launched. geometryOfDisplay, detectFaces,
// and glfwPostEmptyEvent are supplied by test_face_lock_controller.cpp in the
// same test binary and must not be redefined here; the tests below never rely
// on what they return.
extern "C" double glfwGetTime(void)
{
    return 0.0;
}

namespace sidescopes {

bool supportsFaceDetection()
{
    return false;
}

std::vector<DesktopWindow> onScreenWindows(uint32_t)
{
    return {};
}

std::optional<DesktopPoint> globalCursorPosition()
{
    return std::nullopt;
}

std::optional<uint32_t> displayAtPoint(DesktopPoint)
{
    return std::nullopt;
}

std::optional<CapturedImage> captureDisplayImage(uint32_t)
{
    return std::nullopt;
}

bool beginRegionPick(const std::vector<PickerDisplay>&, RegionPickerMode)
{
    return true;
}

RegionPickPoll pollRegionPick()
{
    return {};
}

void cancelRegionPick()
{
}

void setRegionPickMode(RegionPickerMode)
{
}

void setRegionPickChipColor(const std::optional<FloatColor>&)
{
}

void updatePickerFaces(uint32_t, const std::vector<SuggestedRegion>&)
{
}

void hideRegionBorder()
{
}

namespace {

using Catch::Matchers::WithinAbs;
using test::FakeCaptureSource;
using test::makeSolidFrameBuffer;
using test::makeTarget;

constexpr uint32_t StreamedDisplay = 7;

// The picker plus everything it is constructed with, in declaration order so
// the refs it stores outlive nothing. The capture stream is started on a fake
// source so capturedDisplay() names StreamedDisplay - the same display the
// polls below belong to, and the non-zero a pick needs to open.
struct PickerFixture
{
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    RegionPicker picker{capture, worker, source};

    PickerFixture()
    {
        source.targets = {makeTarget(StreamedDisplay, "Test display")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(StreamedDisplay);
        REQUIRE(capture.start());
        REQUIRE(capture.capturedDisplay() == StreamedDisplay);
    }
};

// Publishes @p frame and waits for the worker to consume it, so the pin sample
// reads the frame just published rather than the previous one.
void publishAndAwait(PickerFixture& fix, FrameBuffer&& frame, uint64_t sequence)
{
    fix.mailbox.publish(std::move(frame));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != sequence && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == sequence);
}

}  // namespace

TEST_CASE("A preview poll on the captured display returns the preview region")
{
    PickerFixture fix;

    RegionPickPoll poll;
    poll.active = true;
    poll.displayId = StreamedDisplay;
    poll.preview = RegionOfInterest{10.0, 20.0, 30.0, 40.0};
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, std::nullopt, std::nullopt);

    REQUIRE(outcome.previewRegion.has_value());
    CHECK_THAT(outcome.previewRegion->leftPercent, WithinAbs(10.0, 1e-9));
    CHECK_THAT(outcome.previewRegion->bottomPercent, WithinAbs(40.0, 1e-9));
    // A live preview never ends the pick or confirms anything.
    CHECK_FALSE(outcome.ended);
    CHECK_FALSE(outcome.confirmed.has_value());
    CHECK_FALSE(outcome.cancelled);
}

TEST_CASE("A preview on another display is not fed to the scopes")
{
    PickerFixture fix;

    // Previewing a suggestion on a display the stream is not on would flap the
    // capture on every hover, so it is dropped.
    RegionPickPoll poll;
    poll.active = true;
    poll.displayId = StreamedDisplay + 1;
    poll.preview = RegionOfInterest{10.0, 20.0, 30.0, 40.0};
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, std::nullopt, std::nullopt);

    CHECK_FALSE(outcome.previewRegion.has_value());
}

TEST_CASE("A confirmed draw poll returns the confirmed region and mode")
{
    PickerFixture fix;

    RegionPickPoll poll;
    poll.finished = true;
    poll.displayId = StreamedDisplay;
    poll.confirmed = RegionOfInterest{5.0, 6.0, 55.0, 66.0};
    poll.attachesToWindow = true;
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, std::nullopt, std::nullopt);

    REQUIRE(outcome.confirmed.has_value());
    CHECK_THAT(outcome.confirmed->region.leftPercent, WithinAbs(5.0, 1e-9));
    CHECK_THAT(outcome.confirmed->region.rightPercent, WithinAbs(55.0, 1e-9));
    CHECK(outcome.confirmed->displayId == StreamedDisplay);
    // The in-attach-mode flag rides through so the host knows a rectangle may
    // bind to the window under it.
    CHECK(outcome.confirmed->attachesToWindow);
    CHECK(outcome.ended);
    CHECK_FALSE(outcome.cancelled);
    CHECK_FALSE(fix.picker.active());
}

TEST_CASE("A cancelled poll resets to full screen")
{
    PickerFixture fix;

    // Finished with nothing confirmed and no tool-switch swallow in effect: the
    // user's Esc, which the host turns into a full-screen reset.
    RegionPickPoll poll;
    poll.finished = true;
    poll.displayId = StreamedDisplay;
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, std::nullopt, std::nullopt);

    CHECK(outcome.cancelled);
    CHECK(outcome.ended);
    CHECK_FALSE(outcome.confirmed.has_value());
    CHECK_FALSE(fix.picker.active());
}

TEST_CASE("A cancel swallowed by a tool switch resets nothing")
{
    PickerFixture fix;

    // Open a draw pick, then ask for the pin tool: crossing the region-vs-pin
    // boundary closes the active pick with the swallow set, so its cancel is
    // not the user's Esc.
    fix.picker.request(RegionPickerMode::DrawGlobal);
    const RegionPickOutcome opened = fix.picker.openIfRequested(/*regionIsFullScreen=*/true);
    CHECK(opened.activity);
    REQUIRE(fix.picker.active());

    fix.picker.request(RegionPickerMode::PinColor);
    (void)fix.picker.openIfRequested(true);
    CHECK(fix.picker.active());  // the swallow does not close the pick itself

    // The swallowed cancel then finishes the pick without a reset.
    RegionPickPoll poll;
    poll.finished = true;
    poll.displayId = StreamedDisplay;
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, std::nullopt, std::nullopt);

    CHECK_FALSE(outcome.cancelled);
    CHECK(outcome.ended);
    CHECK_FALSE(fix.picker.active());
}

TEST_CASE("A pin poll returns the sampled colour for the host to pin")
{
    PickerFixture fix;
    fix.worker.start();

    // A solid frame so the point sample reads a known colour.
    const AnalysisWorker::FrameSize frameSize{64, 64};
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{200, 50, 30}, 1), 1);

    RegionPickPoll poll;
    poll.active = true;
    poll.pinMode = true;
    poll.displayId = StreamedDisplay;
    poll.pinnedPoint = DisplayPoint{50.0, 50.0};  // the frame centre
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, frameSize, std::nullopt);

    REQUIRE(outcome.pinColor.has_value());
    CHECK_THAT(outcome.pinColor->r, WithinAbs(200.0f, 1.0f));
    CHECK_THAT(outcome.pinColor->g, WithinAbs(50.0f, 1.0f));
    CHECK_THAT(outcome.pinColor->b, WithinAbs(30.0f, 1.0f));
    // Pinning never previews or confirms a region.
    CHECK_FALSE(outcome.previewRegion.has_value());
    CHECK_FALSE(outcome.confirmed.has_value());

    fix.worker.stop();
}

}  // namespace sidescopes
