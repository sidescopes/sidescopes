#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/capture_controller.h"
#include "app/region_picker.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "platform/desktop.h"
#include "region_overlay_stubs.h"
#include "test_frame.h"

namespace sidescopes {

namespace {

using Catch::Matchers::WithinAbs;
using test::desktopStubs;
using test::FakeCaptureSource;
using test::makeSolidFrameBuffer;
using test::makeTarget;
using test::regionOverlayStubs;

constexpr uint32_t StreamedDisplay = 7;

// The picker plus everything it is constructed with, in declaration order so
// the refs it stores outlive nothing. The capture stream is started on a fake
// source so capturedDisplay() names StreamedDisplay - the same display the
// polls below belong to, and the non-zero a pick needs to open. The scripted
// seams are reset here, since one desktop serves the whole binary.
struct PickerFixture
{
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    RegionPicker picker{capture, worker, source};

    PickerFixture()
    {
        desktopStubs().reset();
        regionOverlayStubs().reset();
        source.targets = {makeTarget(StreamedDisplay, "Test display")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(StreamedDisplay);
        REQUIRE(capture.start());
        REQUIRE(capture.capturedDisplay() == StreamedDisplay);
    }

    ~PickerFixture()
    {
        // The background display scans hold a pointer into the picker, so
        // none may outlive it.
        while (picker.scansRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    PickerFixture(const PickerFixture&) = delete;
    PickerFixture& operator=(const PickerFixture&) = delete;
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

// A frame whose left half is @p left and right half is @p right, so an
// averaged rectangle reports which half it covered.
FrameBuffer makeSplitFrameBuffer(int width, int height, Color left, Color right, uint64_t sequence)
{
    FrameBuffer frame = makeSolidFrameBuffer(width, height, left, sequence);
    for (int py = 0; py < height; ++py) {
        for (int px = width / 2; px < width; ++px) {
            uint8_t* pixel =
                frame.data.data() + static_cast<std::size_t>(py) * frame.strideBytes + static_cast<std::size_t>(px) * 4;
            pixel[0] = right.b;
            pixel[1] = right.g;
            pixel[2] = right.r;
        }
    }

    return frame;
}

// The one-shot grab of a display off the capture stream: square, and a
// different size from the streamed frame so the suggestions it produces
// cannot be confused with the streamed display's.
constexpr int ScannedSide = 200;
constexpr std::size_t ScannedPixels = static_cast<std::size_t>(ScannedSide) * ScannedSide * 4;

// An on-screen window at a rectangle in desktop points.
DesktopWindow makeWindow(uint64_t identity, std::string application, double x, double y, double width, double height)
{
    DesktopWindow window;
    window.x = x;
    window.y = y;
    window.width = width;
    window.height = height;
    window.application = std::move(application);
    window.windowIdentity = identity;
    window.ownerPid = static_cast<int64_t>(identity) * 10;

    return window;
}

// Opens a pick with the region already covering the display, which is the one
// way in: a partial region makes the picker wait out a border-free frame
// against a clock the test binary has frozen.
void openPick(PickerFixture& fix, RegionPickerMode mode)
{
    fix.picker.request(mode);
    const RegionPickOutcome outcome = fix.picker.openIfRequested(/*regionIsFullScreen=*/true);
    REQUIRE(outcome.activity);
    REQUIRE(fix.picker.active());
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
    openPick(fix, RegionPickerMode::DrawGlobal);

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

TEST_CASE("Choosing another region tool switches the open pick's mode")
{
    PickerFixture fix;

    // Both are region tools, so the toolbar keeps working mid-pick: the active
    // picker changes mode instead of a second one stacking on it.
    openPick(fix, RegionPickerMode::DrawGlobal);
    fix.picker.request(RegionPickerMode::AttachWindow);
    (void)fix.picker.openIfRequested(true);

    CHECK(fix.picker.active());
    CHECK(regionOverlayStubs().pickCancels == 0);
    REQUIRE(regionOverlayStubs().lastMode.has_value());
    CHECK(*regionOverlayStubs().lastMode == RegionPickerMode::AttachWindow);
}

TEST_CASE("A pick is not opened while nothing is being captured")
{
    desktopStubs().reset();
    regionOverlayStubs().reset();
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    RegionPicker picker{capture, worker, source};
    REQUIRE(capture.capturedDisplay() == 0);

    // There is no display to draw the overlays over, so the request simply
    // does not open a pick.
    picker.request(RegionPickerMode::DrawGlobal);
    const RegionPickOutcome outcome = picker.openIfRequested(true);

    CHECK_FALSE(outcome.activity);
    CHECK_FALSE(picker.active());
    CHECK_FALSE(regionOverlayStubs().lastMode.has_value());
}

TEST_CASE("A request the overlay refuses is not retried")
{
    PickerFixture fix;
    regionOverlayStubs().pickOpens = false;

    // The overlay could not be shown; the request is spent either way, so the
    // next frame does not try again.
    fix.picker.request(RegionPickerMode::DrawGlobal);
    const RegionPickOutcome first = fix.picker.openIfRequested(true);
    CHECK(first.activity);
    CHECK_FALSE(fix.picker.active());

    regionOverlayStubs().lastMode.reset();
    (void)fix.picker.openIfRequested(true);
    CHECK_FALSE(regionOverlayStubs().lastMode.has_value());
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

TEST_CASE("A dragged pin averages only the rectangle it covered")
{
    PickerFixture fix;
    fix.worker.start();

    // Black on the left, white on the right, so the average reports which half
    // the drag actually read.
    const AnalysisWorker::FrameSize frameSize{64, 64};
    publishAndAwait(fix, makeSplitFrameBuffer(64, 64, Color{0, 0, 0}, Color{255, 255, 255}, 1), 1);

    RegionPickPoll poll;
    poll.active = true;
    poll.pinMode = true;
    poll.displayId = StreamedDisplay;
    poll.pinnedSample = RegionOfInterest{50.0, 0.0, 100.0, 100.0};
    const RegionPickOutcome white = fix.picker.processPoll(poll, frameSize, std::nullopt);
    REQUIRE(white.pinColor.has_value());
    CHECK_THAT(white.pinColor->r, WithinAbs(255.0f, 1.0f));

    poll.pinnedSample = RegionOfInterest{0.0, 0.0, 50.0, 100.0};
    const RegionPickOutcome black = fix.picker.processPoll(poll, frameSize, std::nullopt);
    REQUIRE(black.pinColor.has_value());
    CHECK_THAT(black.pinColor->r, WithinAbs(0.0f, 1.0f));

    // The whole frame is half of each.
    poll.pinnedSample = RegionOfInterest{0.0, 0.0, 100.0, 100.0};
    const RegionPickOutcome both = fix.picker.processPoll(poll, frameSize, std::nullopt);
    REQUIRE(both.pinColor.has_value());
    CHECK_THAT(both.pinColor->r, WithinAbs(127.5f, 2.0f));

    fix.worker.stop();
}

TEST_CASE("A pin drag that covers no pixels pins nothing")
{
    PickerFixture fix;
    fix.worker.start();

    const AnalysisWorker::FrameSize frameSize{64, 64};
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{200, 50, 30}, 1), 1);

    RegionPickPoll poll;
    poll.active = true;
    poll.pinMode = true;
    poll.displayId = StreamedDisplay;
    poll.pinnedSample = RegionOfInterest{40.0, 40.0, 40.0, 60.0};  // no width
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, frameSize, std::nullopt);

    CHECK_FALSE(outcome.pinColor.has_value());
    // Nothing was pinned, but the click still spent the errand.
    CHECK(outcome.activity);

    fix.worker.stop();
}

TEST_CASE("A pin on another display takes the cross-display sample")
{
    PickerFixture fix;
    fix.worker.start();

    // The capture stream holds a frame of its own display; a pin somewhere
    // else must not read it.
    const AnalysisWorker::FrameSize frameSize{64, 64};
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{200, 50, 30}, 1), 1);

    RegionPickPoll poll;
    poll.active = true;
    poll.pinMode = true;
    poll.displayId = StreamedDisplay + 1;
    poll.pinnedPoint = DisplayPoint{50.0, 50.0};
    const FloatColor elsewhere{11.0f, 22.0f, 33.0f};
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, frameSize, elsewhere);

    REQUIRE(outcome.pinColor.has_value());
    CHECK_THAT(outcome.pinColor->r, WithinAbs(11.0f, 1e-3f));
    CHECK_THAT(outcome.pinColor->b, WithinAbs(33.0f, 1e-3f));

    fix.worker.stop();
}

TEST_CASE("A plain pin ends the errand and a kept-open one does not")
{
    PickerFixture fix;
    fix.worker.start();

    const AnalysisWorker::FrameSize frameSize{64, 64};
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{200, 50, 30}, 1), 1);

    RegionPickPoll poll;
    poll.active = true;
    poll.pinMode = true;
    poll.displayId = StreamedDisplay;
    poll.pinnedPoint = DisplayPoint{50.0, 50.0};
    poll.pinnedKeepOpen = true;
    (void)fix.picker.processPoll(poll, frameSize, std::nullopt);
    CHECK(regionOverlayStubs().pickCancels == 0);

    poll.pinnedKeepOpen = false;
    (void)fix.picker.processPoll(poll, frameSize, std::nullopt);
    CHECK(regionOverlayStubs().pickCancels == 1);

    fix.worker.stop();
}

TEST_CASE("The pin tool closing ends the pick without touching the region")
{
    PickerFixture fix;
    fix.worker.start();
    const AnalysisWorker::FrameSize frameSize{64, 64};
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{200, 50, 30}, 1), 1);
    openPick(fix, RegionPickerMode::PinColor);

    // Escape on the pin overlay: the errand is over, and the region the scopes
    // were reading is left exactly as it was.
    RegionPickPoll poll;
    poll.pinMode = true;
    poll.finished = true;
    poll.displayId = StreamedDisplay;
    const RegionPickOutcome outcome = fix.picker.processPoll(poll, frameSize, std::nullopt);

    CHECK(outcome.ended);
    CHECK(outcome.activity);
    CHECK_FALSE(outcome.cancelled);
    CHECK_FALSE(outcome.previewRegion.has_value());
    CHECK_FALSE(outcome.pinColor.has_value());
    CHECK_FALSE(fix.picker.active());

    fix.worker.stop();
}

TEST_CASE("A confirmed window pick resolves back to the window it names")
{
    PickerFixture fix;
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    desktopStubs().onScreenWindows = {makeWindow(42, "Editor", 100.0, 50.0, 400.0, 200.0)};

    openPick(fix, RegionPickerMode::AttachWindow);

    // The window covers 10..50 percent across and 10..50 percent down.
    const RegionOfInterest exact{10.0, 10.0, 50.0, 50.0};
    const RegionPicker::WindowCandidate* matched = fix.picker.matchWindowCandidate(StreamedDisplay, exact);
    REQUIRE(matched != nullptr);
    CHECK(matched->identity == 42);
    CHECK(matched->ownerPid == 420);
    CHECK(matched->application == "Editor");
    CHECK_THAT(matched->windowRect.width, WithinAbs(400.0, 1e-9));

    // The coordinate round-trip may cost a fraction of a percent per edge and
    // still name the same window.
    CHECK(fix.picker.matchWindowCandidate(StreamedDisplay, RegionOfInterest{10.2, 10.2, 50.2, 50.2}) == matched);

    // A rectangle drawn by hand is nobody's window.
    CHECK(fix.picker.matchWindowCandidate(StreamedDisplay, RegionOfInterest{12.0, 12.0, 48.0, 48.0}) == nullptr);
    // Neither is the same rectangle on another display.
    CHECK(fix.picker.matchWindowCandidate(StreamedDisplay + 1, exact) == nullptr);
}

TEST_CASE("The frontmost window under a region's centre is the one it attaches to")
{
    PickerFixture fix;
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    // Frontmost first, both covering the same corner of the display.
    desktopStubs().onScreenWindows = {makeWindow(1, "Front", 0.0, 0.0, 500.0, 250.0),
                                      makeWindow(2, "Behind", 0.0, 0.0, 900.0, 400.0)};

    openPick(fix, RegionPickerMode::AttachFace);

    // A face-sized region inside the overlap belongs to the window in front.
    const RegionPicker::WindowCandidate* front =
        fix.picker.windowContaining(StreamedDisplay, RegionOfInterest{20.0, 20.0, 30.0, 30.0});
    REQUIRE(front != nullptr);
    CHECK(front->identity == 1);

    // Past the front window's edge only the one behind is left.
    const RegionPicker::WindowCandidate* behind =
        fix.picker.windowContaining(StreamedDisplay, RegionOfInterest{60.0, 60.0, 70.0, 70.0});
    REQUIRE(behind != nullptr);
    CHECK(behind->identity == 2);

    // Outside both, nothing is under the region at all.
    CHECK(fix.picker.windowContaining(StreamedDisplay, RegionOfInterest{95.0, 95.0, 99.0, 99.0}) == nullptr);
}

TEST_CASE("The streamed display's faces open with the picker as candidates")
{
    PickerFixture fix;
    fix.worker.start();
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 320.0, 320.0};
    desktopStubs().faceDetectionSupported = true;
    desktopStubs().faces.clear();
    desktopStubs().faces.push_back(IntRect{160, 80, 80, 80});
    publishAndAwait(fix, makeSolidFrameBuffer(640, 640, Color{10, 10, 10}, 1), 1);

    openPick(fix, RegionPickerMode::AttachFace);

    // The detector read the streamed frame at the display's own pixel density.
    const test::DetectorCall detected = desktopStubs().detectorCall();
    CHECK(detected.calls == 1);
    CHECK(detected.width == 640);
    CHECK(detected.height == 640);
    CHECK_THAT(detected.pixelsPerPoint, WithinAbs(2.0f, 1e-6f));

    // The overlay opened with the inset crop of that box as its suggestion.
    REQUIRE(regionOverlayStubs().lastDisplays.size() == 1);
    REQUIRE(regionOverlayStubs().lastDisplays[0].faces.size() == 1);
    CHECK(regionOverlayStubs().lastDisplays[0].facesScanned);
    CHECK_THAT(regionOverlayStubs().lastDisplays[0].faces[0].region.leftPercent, WithinAbs(26.25, 1e-9));

    // Confirming that suggestion resolves back to the detector's own box, which
    // is what a face lock anchors on.
    const FaceCandidate* candidate =
        fix.picker.matchFaceCandidate(StreamedDisplay, regionOverlayStubs().lastDisplays[0].faces[0].region);
    REQUIRE(candidate != nullptr);
    CHECK(candidate->box.x == 160);
    CHECK(candidate->box.width == 80);
    CHECK(candidate->frameWidth == 640);

    // A rectangle nowhere near a face resolves to none.
    CHECK(fix.picker.matchFaceCandidate(StreamedDisplay, RegionOfInterest{0.0, 0.0, 10.0, 10.0}) == nullptr);

    fix.worker.stop();
}

TEST_CASE("A picker opened without face detection scans nothing")
{
    PickerFixture fix;
    fix.worker.start();
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 320.0, 320.0};
    desktopStubs().faces.clear();
    desktopStubs().faces.push_back(IntRect{160, 80, 80, 80});
    publishAndAwait(fix, makeSolidFrameBuffer(640, 640, Color{10, 10, 10}, 1), 1);

    // The platform has no detector, so nothing is scanned and no background
    // scan is spawned - not even a frame's worth of work per display.
    openPick(fix, RegionPickerMode::AttachFace);

    CHECK(desktopStubs().detectorCall().calls == 0);
    REQUIRE(regionOverlayStubs().lastDisplays.size() == 1);
    CHECK(regionOverlayStubs().lastDisplays[0].faces.empty());
    CHECK_FALSE(fix.picker.scansRunning());

    fix.worker.stop();
}

TEST_CASE("A display off the capture stream is scanned in the background")
{
    PickerFixture fix;
    fix.worker.start();
    fix.source.targets.push_back(makeTarget(StreamedDisplay + 1, "Second display"));
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 320.0, 320.0};
    desktopStubs().faceDetectionSupported = true;
    desktopStubs().faces.clear();
    desktopStubs().faces.push_back(IntRect{50, 25, 100, 100});
    desktopStubs().displayImage = CapturedImage{std::vector<uint8_t>(ScannedPixels, 8), ScannedSide, ScannedSide};
    publishAndAwait(fix, makeSolidFrameBuffer(640, 640, Color{10, 10, 10}, 1), 1);

    // The picker opens on the streamed display's faces alone: the other
    // display is scanned without holding the overlay back.
    openPick(fix, RegionPickerMode::AttachFace);
    REQUIRE(regionOverlayStubs().lastDisplays.size() == 2);
    CHECK(regionOverlayStubs().lastDisplays[0].facesScanned);
    CHECK_FALSE(regionOverlayStubs().lastDisplays[1].facesScanned);
    CHECK(regionOverlayStubs().deliveredFaces.empty());

    while (fix.picker.scansRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    fix.picker.drainFaceScans();

    // The landed scan reaches the open overlay, measured on the grabbed
    // frame: the 100-wide box inset a tenth is 30..70 percent of 200 pixels.
    REQUIRE(regionOverlayStubs().deliveredFaces.count(StreamedDisplay + 1) == 1);
    const std::vector<SuggestedRegion>& delivered = regionOverlayStubs().deliveredFaces[StreamedDisplay + 1];
    REQUIRE(delivered.size() == 1);
    CHECK_THAT(delivered[0].region.leftPercent, WithinAbs(30.0, 1e-9));
    CHECK_THAT(delivered[0].region.rightPercent, WithinAbs(70.0, 1e-9));

    // And a pick confirmed there resolves to a candidate on that display.
    const FaceCandidate* candidate = fix.picker.matchFaceCandidate(StreamedDisplay + 1, delivered[0].region);
    REQUIRE(candidate != nullptr);
    CHECK(candidate->displayId == StreamedDisplay + 1);
    CHECK(candidate->frameWidth == ScannedSide);

    // A second drain has nothing left to deliver.
    regionOverlayStubs().deliveredFaces.clear();
    fix.picker.drainFaceScans();
    CHECK(regionOverlayStubs().deliveredFaces.empty());

    fix.worker.stop();
}

TEST_CASE("A scan that lands after its picker closed is dropped")
{
    PickerFixture fix;
    fix.worker.start();
    fix.source.targets.push_back(makeTarget(StreamedDisplay + 1, "Second display"));
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 320.0, 320.0};
    desktopStubs().faceDetectionSupported = true;
    desktopStubs().faces.clear();
    desktopStubs().faces.push_back(IntRect{50, 25, 100, 100});
    desktopStubs().displayImage = CapturedImage{std::vector<uint8_t>(ScannedPixels, 8), ScannedSide, ScannedSide};
    publishAndAwait(fix, makeSolidFrameBuffer(640, 640, Color{10, 10, 10}, 1), 1);

    openPick(fix, RegionPickerMode::AttachFace);
    RegionPickPoll poll;
    poll.finished = true;
    poll.displayId = StreamedDisplay;
    (void)fix.picker.processPoll(poll, std::nullopt, std::nullopt);
    REQUIRE_FALSE(fix.picker.active());

    while (fix.picker.scansRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    fix.picker.drainFaceScans();

    // The overlay is gone; the boxes must not be pushed into whatever opens
    // next, and the finished scan's record is retired.
    CHECK(regionOverlayStubs().deliveredFaces.empty());
    CHECK_FALSE(fix.picker.scansRunning());

    fix.worker.stop();
}

TEST_CASE("The pin chip previews the colour under the cursor")
{
    PickerFixture fix;
    fix.worker.start();
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 64.0, 64.0};
    const AnalysisWorker::FrameSize frameSize{64, 64};
    publishAndAwait(fix, makeSplitFrameBuffer(64, 64, Color{0, 0, 0}, Color{255, 255, 255}, 1), 1);

    RegionPickPoll poll;
    poll.active = true;
    poll.pinMode = true;
    poll.displayId = StreamedDisplay;

    // On the captured display the chip is the frame's own pixel under the
    // cursor - exactly what a click would pin, not an averaged patch.
    desktopStubs().cursor = DesktopPoint{48.0, 32.0};
    desktopStubs().cursorDisplay = StreamedDisplay;
    (void)fix.picker.processPoll(poll, frameSize, std::nullopt);
    REQUIRE(regionOverlayStubs().chipColor.has_value());
    CHECK_THAT(regionOverlayStubs().chipColor->r, WithinAbs(255.0f, 1.0f));

    desktopStubs().cursor = DesktopPoint{16.0, 32.0};
    (void)fix.picker.processPoll(poll, frameSize, std::nullopt);
    REQUIRE(regionOverlayStubs().chipColor.has_value());
    CHECK_THAT(regionOverlayStubs().chipColor->r, WithinAbs(0.0f, 1.0f));

    // On another display the throttled cross-display sample stands in, so the
    // readout keeps working while capture is elsewhere.
    desktopStubs().cursorDisplay = StreamedDisplay + 1;
    (void)fix.picker.processPoll(poll, frameSize, FloatColor{11.0f, 22.0f, 33.0f});
    REQUIRE(regionOverlayStubs().chipColor.has_value());
    CHECK_THAT(regionOverlayStubs().chipColor->g, WithinAbs(22.0f, 1e-3f));

    // With no cursor to read there is nothing to preview.
    desktopStubs().cursor.reset();
    (void)fix.picker.processPoll(poll, frameSize, std::nullopt);
    CHECK_FALSE(regionOverlayStubs().chipColor.has_value());

    fix.worker.stop();
}

}  // namespace sidescopes
