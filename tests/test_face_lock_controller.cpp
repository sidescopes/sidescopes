#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "app/attach_controller.h"
#include "app/capture_controller.h"
#include "app/face_lock.h"
#include "app/face_lock_controller.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "platform/desktop.h"
#include "test_frame.h"

namespace sidescopes {
namespace {

using Catch::Matchers::WithinAbs;
using test::desktopStubs;
using test::FakeCaptureSource;
using test::makeSolidFrameBuffer;
using test::makeTarget;

constexpr uint32_t StreamedDisplay = 3;

// The controller plus everything it is constructed with, in declaration order
// so the refs it stores outlive nothing. The scripted desktop is reset here,
// since one serves the whole binary.
struct ControllerFixture
{
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    AttachController attach;
    FaceLockController controller{attach, worker, capture};

    ControllerFixture()
    {
        desktopStubs().reset();
    }

    ~ControllerFixture()
    {
        // The detection probe holds a pointer into the controller, so it must
        // not outlive it.
        while (controller.probeRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    ControllerFixture(const ControllerFixture&) = delete;
    ControllerFixture& operator=(const ControllerFixture&) = delete;

    // Starts the capture stream on StreamedDisplay, which the probe and the
    // region mapping read the display geometry against.
    void startCapture()
    {
        source.targets = {makeTarget(StreamedDisplay, "Test display")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(StreamedDisplay);
        REQUIRE(capture.start());
    }

    // Makes @p identity an attached window: a lock on a window nothing holds
    // is pruned at the top of every step.
    void attachWindow(uint64_t identity, AttachWindowRect rect)
    {
        (void)attach.attach(identity, 1, "Editor", rect, AttachDisplayRect{0.0, 0.0, 1000.0, 500.0},
                            RegionOfInterest{});
    }
};

// A per-frame verdict naming @p identity as the active window.
AttachDecision decisionFor(uint64_t identity)
{
    AttachDecision decision;
    decision.activeIdentity = identity;

    return decision;
}

// The same verdict with the window's rectangle, which is what a probe needs
// to know where it may search.
AttachDecision attachedDecision(uint64_t identity, AttachWindowRect rect)
{
    AttachDecision decision = decisionFor(identity);
    decision.activeRect = rect;

    return decision;
}

// A forehead-style lock on a face at 500,300 sized 200: the crop is half the
// anchor width, centred on it.
FaceLockState foreheadLock()
{
    return face_lock::makeLock(FaceAnchor{500.0, 300.0, 200.0}, LockRect{450.0, 250.0, 550.0, 350.0});
}

// Publishes @p frame and waits for the worker to consume it, so the next
// content probe reads the frame just published rather than the previous one.
void publishAndAwait(ControllerFixture& fix, FrameBuffer&& frame, uint64_t sequence)
{
    fix.mailbox.publish(std::move(frame));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != sequence && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == sequence);
}

// A square frame whose top-left quarter is @p inside and the rest @p outside,
// so a region watch can be shown to read one and not the other.
FrameBuffer makeQuarteredFrameBuffer(int side, Color inside, Color outside, uint64_t sequence)
{
    FrameBuffer frame = test::makeSolidFrameBuffer(side, side, outside, sequence);
    for (int py = 0; py < side / 2; ++py) {
        for (int px = 0; px < side / 2; ++px) {
            uint8_t* pixel =
                frame.data.data() + static_cast<std::size_t>(py) * frame.strideBytes + static_cast<std::size_t>(px) * 4;
            pixel[0] = inside.b;
            pixel[1] = inside.g;
            pixel[2] = inside.r;
        }
    }

    return frame;
}

// A frame whose every pixel spells its own coordinates: blue and green carry
// the column, red the row. The crop a probe copies out then says exactly which
// pixels it took, not only how many.
FrameBuffer makeCoordinateFrameBuffer(int width, int height, uint64_t sequence)
{
    FrameBuffer frame = test::makeSolidFrameBuffer(width, height, Color{0, 0, 0}, sequence);
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            uint8_t* pixel =
                frame.data.data() + static_cast<std::size_t>(py) * frame.strideBytes + static_cast<std::size_t>(px) * 4;
            pixel[0] = static_cast<uint8_t>(px & 0xFF);
            pixel[1] = static_cast<uint8_t>((px >> 8) & 0xFF);
            pixel[2] = static_cast<uint8_t>(py & 0xFF);
        }
    }

    return frame;
}

// The frame coordinate a recorded crop started at, read back out of its first
// pixel. Rows are read modulo 256, which every case below stays inside.
std::pair<int, int> cropOrigin(const test::DetectorCall& detected)
{
    return {detected.firstPixel[0] + (detected.firstPixel[1] << 8), detected.firstPixel[2]};
}

// Waits out the detached detection probe, so the crop it was handed can be
// read back.
void awaitProbe(const ControllerFixture& fix)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.controller.probeRunning() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(fix.controller.probeRunning());
}

}  // namespace

TEST_CASE("A probe on the anchor holds the region and is not hunting")
{
    ControllerFixture fix;
    fix.controller.addLock(1, foreheadLock(), 0.0);

    // A detector box centred exactly on the anchor: 400,200 sized 200x200.
    const FaceLockOutcome outcome = fix.controller.ingestProbeResult(
        {IntRect{400, 200, 200, 200}}, IntRect{0, 0, 1000, 800}, 1, decisionFor(1), 1, std::nullopt, 1.0);

    CHECK_FALSE(fix.controller.hunting());
    // A candidate on the anchor is a micro-move: it holds, it does not adopt.
    CHECK_FALSE(outcome.applyRegion.has_value());
    CHECK_FALSE(outcome.lostLock.has_value());
    CHECK(fix.controller.contains(1));
}

TEST_CASE("A settled move adopts and maps the region through the window")
{
    ControllerFixture fix;
    // A frame twice the display's point size, with a full-window display and
    // window so the mapping is easy to read back.
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    desktopStubs().windowGeometry = WindowGeometry{0.0, 0.0, 1000.0, 500.0, false, ""};
    const AnalysisWorker::FrameSize frameSize{2000, 1000};
    fix.controller.addLock(1, foreheadLock(), 0.0);

    // The first probe at the moved position is unsettled: hunting, no region.
    const FaceLockOutcome first = fix.controller.ingestProbeResult(
        {IntRect{460, 200, 200, 200}}, IntRect{0, 0, 1000, 800}, 1, decisionFor(1), 1, frameSize, 1.0);
    CHECK(fix.controller.hunting());
    CHECK_FALSE(first.applyRegion.has_value());

    // The second agreeing probe adopts, and the mapped region comes back.
    const FaceLockOutcome second = fix.controller.ingestProbeResult(
        {IntRect{460, 200, 200, 200}}, IntRect{0, 0, 1000, 800}, 1, decisionFor(1), 1, frameSize, 2.0);
    CHECK_FALSE(fix.controller.hunting());
    REQUIRE(second.applyRegion.has_value());
    // The forehead crop (0.5 anchor widths, centred) at anchor 560,300 width
    // 200 is 510,250..610,350 in frame pixels; over a 2000x1000 frame that is
    // 25.5,25..30.5,35 percent, and no attachment reshapes it.
    CHECK_THAT(second.applyRegion->leftPercent, WithinAbs(25.5, 1e-6));
    CHECK_THAT(second.applyRegion->topPercent, WithinAbs(25.0, 1e-6));
    CHECK_THAT(second.applyRegion->rightPercent, WithinAbs(30.5, 1e-6));
    CHECK_THAT(second.applyRegion->bottomPercent, WithinAbs(35.0, 1e-6));
    CHECK_FALSE(second.lostLock.has_value());
}

TEST_CASE("Persistent absence gives the lock up and reports it lost")
{
    ControllerFixture fix;
    fix.controller.addLock(7, foreheadLock(), 0.0);

    // A box flush against the ROI's left edge is never trusted, so every probe
    // is an empty sighting and the give-up clock runs to the end.
    const IntRect roi{0, 0, 1000, 800};
    const IntRect edgeClipped{0, 300, 100, 200};
    FaceLockOutcome outcome;
    for (int probe = 0; probe < 16; ++probe) {
        CHECK(fix.controller.contains(7));
        CHECK_FALSE(outcome.lostLock.has_value());
        outcome = fix.controller.ingestProbeResult({edgeClipped}, roi, 7, decisionFor(7), 7, std::nullopt, 1.0);
    }
    REQUIRE(outcome.lostLock.has_value());
    CHECK(*outcome.lostLock == 7);
    CHECK_FALSE(outcome.applyRegion.has_value());
    CHECK_FALSE(fix.controller.contains(7));
    CHECK_FALSE(fix.controller.hunting());
}

TEST_CASE("A probe for a window that is no longer active is ignored")
{
    ControllerFixture fix;
    fix.controller.addLock(1, foreheadLock(), 0.0);

    // The probe searched window 1, but the active window is now 2.
    AttachDecision decision;
    decision.activeIdentity = 2;
    const FaceLockOutcome outcome = fix.controller.ingestProbeResult(
        {IntRect{400, 200, 200, 200}}, IntRect{0, 0, 1000, 800}, 1, decision, 2, std::nullopt, 1.0);

    CHECK_FALSE(outcome.applyRegion.has_value());
    CHECK_FALSE(outcome.lostLock.has_value());
    CHECK(fix.controller.contains(1));      // the lock is untouched
    CHECK_FALSE(fix.controller.hunting());  // and no verdict was recorded
}

TEST_CASE("The content watch flags a change and clears after the settle time")
{
    ControllerFixture fix;
    fix.worker.start();

    const RegionOfInterest region;  // the whole frame
    const AnalysisWorker::FrameSize frameSize{64, 64};

    // A baseline on a black frame: nothing has changed yet.
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{0, 0, 0}, 1), 1);
    fix.controller.probeContentChange(region, frameSize, 0.0);
    CHECK_FALSE(fix.controller.contentUnsettled(0.0));

    // A second identical frame stays settled: the threshold ignores noise.
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{0, 0, 0}, 2), 2);
    fix.controller.probeContentChange(region, frameSize, 1.0);
    CHECK_FALSE(fix.controller.contentUnsettled(1.0));

    // A very different frame is a change, stamped at t = 2.0.
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{255, 255, 255}, 3), 3);
    fix.controller.probeContentChange(region, frameSize, 2.0);
    CHECK(fix.controller.contentUnsettled(2.0));
    CHECK(fix.controller.contentUnsettled(2.44));        // within the 0.45s settle
    CHECK_FALSE(fix.controller.contentUnsettled(2.45));  // at the edge, settled again
    CHECK_FALSE(fix.controller.contentUnsettled(2.5));

    fix.worker.stop();
}

TEST_CASE("The content watch reads the locked region and nothing outside it")
{
    ControllerFixture fix;
    fix.worker.start();

    // The locked region is the frame's top-left quarter.
    const RegionOfInterest region{0.0, 0.0, 50.0, 50.0};
    const AnalysisWorker::FrameSize frameSize{64, 64};

    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{0, 0, 0}, 1), 1);
    fix.controller.probeContentChange(region, frameSize, 0.0);
    CHECK_FALSE(fix.controller.contentUnsettled(0.0));

    // Repainting the rest of the frame is somebody else's window moving; the
    // locked content did not change and the border stays up.
    publishAndAwait(fix, makeQuarteredFrameBuffer(64, Color{0, 0, 0}, Color{255, 255, 255}, 2), 2);
    fix.controller.probeContentChange(region, frameSize, 1.0);
    CHECK_FALSE(fix.controller.contentUnsettled(1.0));

    // Repainting inside it is the pan the border must hide for.
    publishAndAwait(fix, makeQuarteredFrameBuffer(64, Color{255, 255, 255}, Color{255, 255, 255}, 3), 3);
    fix.controller.probeContentChange(region, frameSize, 2.0);
    CHECK(fix.controller.contentUnsettled(2.0));

    fix.worker.stop();
}

TEST_CASE("A region the host itself moved only rebaselines the content watch")
{
    ControllerFixture fix;
    fix.worker.start();

    const AnalysisWorker::FrameSize frameSize{64, 64};
    publishAndAwait(fix, makeQuarteredFrameBuffer(64, Color{0, 0, 0}, Color{255, 255, 255}, 1), 1);
    fix.controller.probeContentChange(RegionOfInterest{0.0, 0.0, 50.0, 50.0}, frameSize, 0.0);

    // The same frame read through a different rectangle samples wholly
    // different pixels; that is the region moving, not the content changing.
    fix.controller.probeContentChange(RegionOfInterest{50.0, 50.0, 100.0, 100.0}, frameSize, 1.0);
    CHECK_FALSE(fix.controller.contentUnsettled(1.0));

    fix.worker.stop();
}

TEST_CASE("The probe searches the patch around the anchor")
{
    ControllerFixture fix;
    fix.startCapture();
    fix.worker.start();
    // The display is 1000 points wide against a 2000 pixel frame, so a point
    // is two pixels and the window covers the whole frame.
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    publishAndAwait(fix, makeCoordinateFrameBuffer(2000, 1000, 1), 1);
    fix.attachWindow(1, AttachWindowRect{0.0, 0.0, 1000.0, 500.0});

    // A 100-wide anchor at 500,400 reaches 2.5 anchor widths each way.
    fix.controller.addLock(
        1, face_lock::makeLock(FaceAnchor{500.0, 400.0, 100.0}, LockRect{450.0, 350.0, 550.0, 450.0}), 0.0);
    const FaceLockOutcome outcome = fix.controller.update(
        attachedDecision(1, AttachWindowRect{0.0, 0.0, 1000.0, 500.0}), AnalysisWorker::FrameSize{2000, 1000}, 1,
        RegionOfInterest{}, /*gestureActive=*/false, 1.0);
    CHECK_FALSE(outcome.applyRegion.has_value());
    awaitProbe(fix);

    const test::DetectorCall detected = desktopStubs().detectorCall();
    CHECK(detected.calls == 1);
    CHECK(cropOrigin(detected) == std::pair<int, int>{250, 150});
    CHECK(detected.width == 500);   // 250..750 across
    CHECK(detected.height == 500);  // 150..650 down
    CHECK_THAT(detected.pixelsPerPoint, WithinAbs(2.0f, 1e-6f));

    fix.worker.stop();
}

TEST_CASE("The probe never searches past the attached window")
{
    ControllerFixture fix;
    fix.startCapture();
    fix.worker.start();
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    publishAndAwait(fix, makeCoordinateFrameBuffer(2000, 1000, 1), 1);
    fix.attachWindow(1, AttachWindowRect{0.0, 0.0, 200.0, 500.0});

    // The window occupies the left 200 points - 400 pixels - so the anchor's
    // own reach runs past it and a neighbouring window's faces stay out of
    // range.
    fix.controller.addLock(
        1, face_lock::makeLock(FaceAnchor{300.0, 400.0, 100.0}, LockRect{250.0, 350.0, 350.0, 450.0}), 0.0);
    (void)fix.controller.update(attachedDecision(1, AttachWindowRect{0.0, 0.0, 200.0, 500.0}),
                                AnalysisWorker::FrameSize{2000, 1000}, 1, RegionOfInterest{}, false, 1.0);
    awaitProbe(fix);

    const test::DetectorCall detected = desktopStubs().detectorCall();
    CHECK(detected.calls == 1);
    CHECK(cropOrigin(detected) == std::pair<int, int>{50, 150});
    CHECK(detected.width == 350);   // 50..400, the window's own right edge
    CHECK(detected.height == 500);  // 150..650, well inside the frame

    fix.worker.stop();
}

TEST_CASE("A same-size window move carries the lock's search with it")
{
    ControllerFixture fix;
    fix.startCapture();
    fix.worker.start();
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    publishAndAwait(fix, makeCoordinateFrameBuffer(2000, 1000, 1), 1);

    const AnalysisWorker::FrameSize frameSize{2000, 1000};
    fix.attachWindow(1, AttachWindowRect{100.0, 0.0, 400.0, 500.0});
    fix.controller.addLock(1,
                           face_lock::makeLock(FaceAnchor{500.0, 400.0, 100.0}, LockRect{450.0, 350.0, 550.0, 450.0}),
                           0.0, AttachWindowRect{0.0, 0.0, 400.0, 500.0});

    // The window slides 100 points right, which is 200 frame pixels: the
    // anchor rides along, so the searched patch does too. It stays the same
    // size, which a lock that had not moved could not manage - the window's
    // right edge would have clipped it.
    (void)fix.controller.update(attachedDecision(1, AttachWindowRect{100.0, 0.0, 400.0, 500.0}), frameSize, 1,
                                RegionOfInterest{}, false, 1.0);
    awaitProbe(fix);

    const test::DetectorCall detected = desktopStubs().detectorCall();
    CHECK(detected.calls == 1);
    CHECK(cropOrigin(detected) == std::pair<int, int>{450, 150});
    CHECK(detected.width == 500);   // 450..950, 2.5 anchor widths around 700
    CHECK(detected.height == 500);  // 150..650, the anchor's row did not move

    fix.worker.stop();
}

TEST_CASE("No probe runs while the user is dragging the window")
{
    ControllerFixture fix;
    fix.startCapture();
    fix.worker.start();
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    publishAndAwait(fix, makeCoordinateFrameBuffer(2000, 1000, 1), 1);
    fix.attachWindow(1, AttachWindowRect{0.0, 0.0, 1000.0, 500.0});
    fix.controller.addLock(1, foreheadLock(), 0.0);

    // The drag wins; the lock catches up once things settle.
    (void)fix.controller.update(attachedDecision(1, AttachWindowRect{0.0, 0.0, 1000.0, 500.0}),
                                AnalysisWorker::FrameSize{2000, 1000}, 1, RegionOfInterest{},
                                /*gestureActive=*/true, 1.0);

    CHECK_FALSE(fix.controller.probeRunning());
    CHECK(desktopStubs().detectorCall().calls == 0);

    fix.worker.stop();
}

TEST_CASE("A lock whose window is no longer attached is dropped")
{
    ControllerFixture fix;
    fix.controller.addLock(1, foreheadLock(), 0.0);
    REQUIRE(fix.controller.contains(1));

    // Nothing was ever attached, so the step prunes the lock and the hunting
    // state with it.
    AttachDecision decision;
    const FaceLockOutcome outcome = fix.controller.update(decision, std::nullopt, 0, RegionOfInterest{}, false, 1.0);

    CHECK_FALSE(fix.controller.contains(1));
    CHECK_FALSE(fix.controller.hunting());
    // A pruned lock is not a lost one: the host already knows the window went.
    CHECK_FALSE(outcome.lostLock.has_value());
}

TEST_CASE("Activating a window whose anchor went stale starts a hunt")
{
    ControllerFixture fix;
    fix.controller.addLock(1, foreheadLock(), /*verifiedAt=*/1.0);

    // Verified within the probe period: the border keeps its region through a
    // quick focus flip.
    fix.controller.onActivated(1, 1.2);
    CHECK_FALSE(fix.controller.hunting());

    // Older than a probe period: the border holds until the first fresh
    // verdict rather than flashing a stale rectangle.
    fix.controller.onActivated(1, 2.0);
    CHECK(fix.controller.hunting());

    // A window that holds no lock changes nothing.
    fix.controller.clear();
    fix.controller.onActivated(99, 9.0);
    CHECK_FALSE(fix.controller.hunting());
}

TEST_CASE("Clearing and removing forget the locks they are given")
{
    ControllerFixture fix;
    fix.controller.addLock(1, foreheadLock(), 0.0);
    fix.controller.addLock(2, foreheadLock(), 0.0);

    fix.controller.removeLock(1);
    CHECK_FALSE(fix.controller.contains(1));
    CHECK(fix.controller.contains(2));

    // Watch Full Screen drops everything, the hunting state included.
    fix.controller.onActivated(2, 5.0);
    REQUIRE(fix.controller.hunting());
    fix.controller.clear();
    CHECK_FALSE(fix.controller.contains(2));
    CHECK_FALSE(fix.controller.hunting());
}

TEST_CASE("A border edit re-teaches the crop the face carries")
{
    ControllerFixture fix;
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    desktopStubs().windowGeometry = WindowGeometry{0.0, 0.0, 1000.0, 500.0, false, ""};
    const AnalysisWorker::FrameSize frameSize{2000, 1000};
    fix.controller.addLock(1, foreheadLock(), 0.0);

    // The user drags the border to a different rectangle over the same face:
    // 400,200..600,400 in frame pixels, which is 20,20..30,40 percent.
    fix.controller.rebindCrop(1, RegionOfInterest{20.0, 20.0, 30.0, 40.0}, frameSize);

    // Two agreeing probes at a moved anchor adopt, and the region that comes
    // back is the new crop carried to the new position - 60 pixels right of
    // where it was taught.
    const IntRect moved{460, 200, 200, 200};
    (void)fix.controller.ingestProbeResult({moved}, IntRect{0, 0, 1000, 800}, 1, decisionFor(1), 1, frameSize, 1.0);
    const FaceLockOutcome adopted =
        fix.controller.ingestProbeResult({moved}, IntRect{0, 0, 1000, 800}, 1, decisionFor(1), 1, frameSize, 2.0);

    REQUIRE(adopted.applyRegion.has_value());
    CHECK_THAT(adopted.applyRegion->leftPercent, WithinAbs(23.0, 1e-6));
    CHECK_THAT(adopted.applyRegion->rightPercent, WithinAbs(33.0, 1e-6));
    CHECK_THAT(adopted.applyRegion->topPercent, WithinAbs(20.0, 1e-6));
    CHECK_THAT(adopted.applyRegion->bottomPercent, WithinAbs(40.0, 1e-6));

    // An edit aimed at a window that holds no lock is simply ignored.
    fix.controller.rebindCrop(99, RegionOfInterest{0.0, 0.0, 1.0, 1.0}, frameSize);
    CHECK(fix.controller.contains(1));
}

TEST_CASE("An adopted anchor maps to nothing while the window is out of reach")
{
    ControllerFixture fix;
    const AnalysisWorker::FrameSize frameSize{2000, 1000};
    fix.controller.addLock(1, foreheadLock(), 0.0);
    const IntRect moved{460, 200, 200, 200};
    (void)fix.controller.ingestProbeResult({moved}, IntRect{0, 0, 1000, 800}, 1, decisionFor(1), 1, frameSize, 1.0);

    // A minimized window has no rectangle to map through, so the adoption
    // stands in the lock and nothing is asked of the host.
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    desktopStubs().windowGeometry = WindowGeometry{0.0, 0.0, 1000.0, 500.0, true, ""};
    const FaceLockOutcome minimized =
        fix.controller.ingestProbeResult({moved}, IntRect{0, 0, 1000, 800}, 1, decisionFor(1), 1, frameSize, 2.0);
    CHECK_FALSE(minimized.applyRegion.has_value());
    CHECK_FALSE(fix.controller.hunting());  // the anchor was still confirmed
}

}  // namespace sidescopes
