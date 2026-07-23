#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "app/attach_controller.h"
#include "app/capture_controller.h"
#include "app/face_lock.h"
#include "app/face_lock_controller.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "fake_capture.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"
#include "test_frame.h"

// The controller reaches the platform desktop and detection seams and the GLFW
// wake directly, but the test binary links neither a desktop backend nor a
// windowing library. It provides its own: the geometry doubles are settable so
// the adopt path can map a real region, and detection is never reached through
// the injectable ingest seam.
extern "C" void glfwPostEmptyEvent(void)
{
}

namespace sidescopes {

namespace {
std::optional<WindowGeometry> g_windowGeometry;
std::optional<DisplayGeometry> g_displayGeometry;
}  // namespace

std::optional<WindowGeometry> windowGeometry(uint64_t)
{
    return g_windowGeometry;
}

std::optional<DisplayGeometry> geometryOfDisplay(uint32_t)
{
    return g_displayGeometry;
}

std::vector<IntRect> detectFaces(const FrameView&, float)
{
    return {};
}

namespace {

using Catch::Matchers::WithinAbs;
using test::FakeCaptureSource;
using test::makeSolidFrameBuffer;

// The controller plus everything it is constructed with, in declaration order
// so the refs it stores outlive nothing.
struct ControllerFixture
{
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    AttachController attach;
    FaceLockController controller{attach, worker, capture};
};

// A per-frame verdict naming @p identity as the active window.
AttachDecision decisionFor(uint64_t identity)
{
    AttachDecision decision;
    decision.activeIdentity = identity;

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
    g_displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    g_windowGeometry = WindowGeometry{0.0, 0.0, 1000.0, 500.0, false, ""};
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

}  // namespace sidescopes
