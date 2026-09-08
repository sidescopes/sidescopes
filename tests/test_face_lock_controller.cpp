#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "app/attach_controller.h"
#include "app/capture_controller.h"
#include "app/face_lock_controller.h"
#include "core/analysis_worker.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "test_frame.h"

namespace sidescopes {
namespace {

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinULP;
using test::desktopStubs;
constexpr uint32_t StreamedDisplay = 3;
constexpr AttachWindowRect FullWindow{0.0, 0.0, 1000.0, 500.0};
constexpr FaceAnchor InitialAnchor{500.0, 250.0, 100.0};
constexpr RegionOfInterest InitialRegion{47.5, 45.0, 52.5, 55.0};

void checkSameCrop(const RegionOfInterest& actual, const RegionOfInterest& expected, int width, int height)
{
    // The attachment stores desktop points, so its percentages can differ
    // by round-off. The crop must still select precisely the same pixels.
    CHECK_THAT(actual.leftPercent, WithinULP(expected.leftPercent, 4));
    CHECK_THAT(actual.topPercent, WithinULP(expected.topPercent, 4));
    CHECK_THAT(actual.rightPercent, WithinULP(expected.rightPercent, 4));
    CHECK_THAT(actual.bottomPercent, WithinULP(expected.bottomPercent, 4));
    const auto actualPixels = actual.toPixels(width, height);
    const auto expectedPixels = expected.toPixels(width, height);
    CAPTURE(actualPixels.x, actualPixels.y, actualPixels.width, actualPixels.height);
    CAPTURE(expectedPixels.x, expectedPixels.y, expectedPixels.width, expectedPixels.height);
    CHECK(actualPixels == expectedPixels);
}

FaceLockState foreheadLock(FaceAnchor anchor = InitialAnchor)
{
    const double quarter = anchor.width / 4.0;
    return face_lock::makeLock(anchor, {anchor.centerX - quarter, anchor.centerY - quarter, anchor.centerX + quarter,
                                        anchor.centerY + quarter});
}

struct ControllerFixture
{
    double clock = 1000.0;
    test::FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    AttachController attach;
    FaceLockController controller{attach, worker, capture, [this] { return clock; }};
    AttachDecision decision;
    AnalysisWorker::FrameSize frameSize{1000, 500, 1000, 500};
    AnalysisSettings settings;
    std::optional<RegionOfInterest> region = InitialRegion;
    bool gesture = false;
    AnalysisWorker::Output output;
    uint64_t seen = 0;

    ControllerFixture()
    {
        desktopStubs().reset();
        desktopStubs().faceDetectionSupported = true;
        desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
        source.targets = {test::makeTarget(StreamedDisplay, "Test display")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(StreamedDisplay);
        REQUIRE(capture.start());
        settings.region = region;
        settings.enabledScopes = {"org.sidescopes.histogram"};
        worker.updateSettings(settings);
        worker.startInline();
        setFace(InitialAnchor);
    }

    ~ControllerFixture()
    {
        worker.stop();
        desktopStubs().beforeDetection = {};
        desktopStubs().sessionDetection = {};
    }

    static void setFace(FaceAnchor face)
    {
        desktopStubs().sessionDetection = [face](const FrameView& crop, double) {
            return FaceDetectionResult{FaceDetectionStatus::Completed,
                                       {{static_cast<int>(face.centerX - face.width / 2.0) - crop.sourceX,
                                         static_cast<int>(face.centerY - face.width / 2.0) - crop.sourceY,
                                         static_cast<int>(face.width), static_cast<int>(face.width)}}};
        };
    }

    void select(FaceLockState state = foreheadLock(), AttachWindowRect rect = FullWindow,
                std::optional<AttachWindowRect> previous = std::nullopt)
    {
        (void)attach.attach(1, 1, "Editor", rect, AttachDisplayRect{0, 0, 1000, 500}, *region);
        controller.addLock(1, state, previous.value_or(rect));
        decision.activeIdentity = 1;
        decision.activeRect = rect;
        (void)tick();
    }

    FaceLockOutcome tick(std::optional<double> now = std::nullopt)
    {
        auto result = controller.update(decision, frameSize, gesture, now.value_or(clock));
        if (result.applyRegion) {
            region = result.applyRegion;
        }
        if (settings.selectionRevision != controller.selectionRevision()) {
            settings.selectionRevision = controller.selectionRevision();
            settings.region = region;
            worker.updateSettings(settings);
        }
        return result;
    }

    FrameBuffer frame(uint64_t sequence, Color color = {50, 100, 150}) const
    {
        auto result = test::makeSolidFrameBuffer(frameSize.width, frameSize.height, color, sequence);
        result.stamp = {capture.streamEpoch(), StreamedDisplay, clock};
        return result;
    }

    void submit(FrameBuffer frame)
    {
        // Pixel construction and sanitizer overhead are not source time.
        // Advance one deterministic video interval for each submitted frame.
        clock += 0.04;
        frame.stamp.receivedSeconds = clock;
        const uint64_t sequence = frame.sequence;
        mailbox.publish(std::move(frame));
        worker.pump();
        REQUIRE(worker.consumedFrameSequence() == sequence);
    }

    FaceLockOutcome advance(uint64_t sequence, Color color = {50, 100, 150})
    {
        (void)tick();
        submit(frame(sequence, color));
        return tick();
    }

    bool fetch()
    {
        return worker.fetchOutput(seen, output, controller.selectionRevision());
    }
};

FrameBuffer coordinateFrame(const ControllerFixture& fixture, uint64_t sequence)
{
    auto frame = fixture.frame(sequence);
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            auto* pixel =
                frame.data.data() + static_cast<std::size_t>(y) * frame.strideBytes + static_cast<std::size_t>(x) * 4;
            pixel[0] = static_cast<uint8_t>(x & 255);
            pixel[1] = static_cast<uint8_t>((x >> 8) & 255);
            pixel[2] = static_cast<uint8_t>(y & 255);
        }
    }
    frame.stamp.receivedSeconds = fixture.clock;
    return frame;
}

std::pair<int, int> cropOrigin(const test::DetectorCall& call)
{
    return {call.firstPixel[0] + (call.firstPixel[1] << 8), call.firstPixel[2]};
}

}  // namespace

TEST_CASE("An attachment coordinate round trip preserves the exact face crop pixels")
{
    AttachController attachment;
    const AttachDisplayRect display{0, 0, 1000, 500};
    (void)attachment.attach(1, 1, "Editor", FullWindow, display, InitialRegion);
    // These positions exercise the formerly lost rightmost pixel after
    // percentages were converted to stored points and emitted again.
    for (int movement : {8, 24, 40}) {
        CAPTURE(movement);
        const RegionOfInterest selected{(475.0 + movement) / 10.0, 45, (525.0 + movement) / 10.0, 55};
        const auto restored = attachment.editRegion(selected, FullWindow, display);
        checkSameCrop(selected, restored, 1000, 500);
        CHECK(selected.toPixels(1000, 500) == IntRect{475 + movement, 225, 50, 50});
        CHECK(restored.toPixels(1000, 500) == IntRect{475 + movement, 225, 50, 50});
        // The same source region must stay equivalent on a Retina capture.
        checkSameCrop(selected, restored, 2000, 1000);
        CHECK(restored.toPixels(2000, 1000) == IntRect{950 + movement * 2, 450, 100, 100});
    }
}

TEST_CASE("A face moves on every video frame without waiting for content to settle")
{
    ControllerFixture fixture;
    fixture.select();
    double previousLeft = InitialRegion.leftPercent;
    for (uint64_t sequence = 1; sequence <= 5; ++sequence) {
        fixture.setFace({500.0 + 8.0 * static_cast<double>(sequence), 250.0, 100.0});
        const auto result = fixture.advance(sequence, sequence % 2 ? Color{0, 0, 0} : Color{255, 255, 255});
        REQUIRE(result.applyRegion);
        CHECK_FALSE(result.lostLock);
        const double detectedLeft = 47.5 + 0.8 * static_cast<double>(sequence);
        CHECK(result.applyRegion->leftPercent > previousLeft);
        CHECK(result.applyRegion->leftPercent <= detectedLeft + 1e-9);
        // Each mapped edge stays within2% of the100-pixel face width.
        CHECK_THAT(result.applyRegion->leftPercent, WithinAbs(detectedLeft, 0.200001));
        previousLeft = result.applyRegion->leftPercent;
        REQUIRE(fixture.fetch());
        REQUIRE(fixture.output.images.contains("org.sidescopes.histogram"));
        CHECK_FALSE(fixture.output.images.at("org.sidescopes.histogram").rgba.empty());
        REQUIRE(fixture.output.region);
        INFO("source sequence=" << sequence);
        checkSameCrop(*fixture.output.region, *result.applyRegion, fixture.frameSize.width, fixture.frameSize.height);
        CHECK(fixture.output.frameSequence == sequence);
        CHECK(fixture.output.frameStamp.captureEpoch == fixture.capture.streamEpoch());
        CHECK(fixture.output.selectionRevision == fixture.controller.selectionRevision());
    }
    CHECK(desktopStubs().detectorCall().calls == 5);
}

TEST_CASE("Scope changes reuse a still photo and the first later frame is detected")
{
    ControllerFixture fixture;
    fixture.select();
    (void)fixture.advance(1);
    fixture.settings.sampleThinning = 2;
    fixture.worker.updateSettings(fixture.settings);
    fixture.worker.pump();
    const auto quiet = fixture.tick(fixture.clock + 60.0);
    CHECK_FALSE(quiet.lostLock);
    CHECK(fixture.controller.contains(1));
    CHECK(desktopStubs().detectorCall().calls == 1);
    fixture.setFace({508, 250, 100});
    REQUIRE(fixture.advance(2).applyRegion);
    CHECK(desktopStubs().detectorCall().calls == 2);
}

TEST_CASE("A transient uncertain frame keeps the last accepted crop and can recover immediately")
{
    ControllerFixture fixture;
    fixture.select();
    fixture.setFace({520, 250, 100});
    REQUIRE(fixture.advance(1).applyRegion);
    const auto last = fixture.region;
    desktopStubs().sessionDetection = {};
    desktopStubs().faces.clear();
    SECTION("successful absence")
    {
        desktopStubs().detectionStatus = FaceDetectionStatus::Completed;
    }
    SECTION("native failure")
    {
        desktopStubs().detectionStatus = FaceDetectionStatus::Failed;
    }
    const auto uncertain = fixture.advance(2, {200, 100, 50});
    REQUIRE(uncertain.applyRegion);
    CHECK_FALSE(uncertain.lostLock);
    CHECK(fixture.region == last);
    CHECK(fixture.controller.contains(1));
    REQUIRE(fixture.fetch());
    REQUIRE(fixture.output.region);
    REQUIRE(last);
    checkSameCrop(*fixture.output.region, *last, fixture.frameSize.width, fixture.frameSize.height);
    CHECK(fixture.output.frameSequence == 2u);
    fixture.setFace({530, 250, 100});
    const auto recovered = fixture.advance(3);
    REQUIRE(recovered.applyRegion);
    CHECK_THAT(recovered.applyRegion->leftPercent, WithinAbs(50.5, 1e-6));
    CHECK_FALSE(recovered.lostLock);
}

TEST_CASE("Capture silence suppresses expired face readings while retaining the selection")
{
    ControllerFixture fixture;
    fixture.select();
    fixture.setFace({508, 250, 100});
    REQUIRE(fixture.advance(1).applyRegion);
    const auto last = fixture.region;
    desktopStubs().sessionDetection = {};
    desktopStubs().faces.clear();
    (void)fixture.advance(2);
    REQUIRE(fixture.fetch());
    CHECK_FALSE(fixture.output.suppressed);
    const auto generation = fixture.output.readingGeneration;
    const double loss = fixture.clock;
    fixture.clock = loss + 0.999;
    CHECK_FALSE(fixture.tick().lostLock);
    REQUIRE(fixture.controller.readingState(1));
    CHECK_FALSE(fixture.controller.readingState(1)->searching);

    fixture.clock = loss + 1.0;
    const auto searching = fixture.tick();
    CHECK_FALSE(searching.lostLock);
    CHECK_FALSE(searching.applyRegion);
    REQUIRE(fixture.controller.readingState(1));
    CHECK(fixture.controller.readingState(1)->searching);
    CHECK(fixture.controller.contains(1));
    CHECK(fixture.attach.isAttached(1));
    CHECK(fixture.region == last);
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.suppressed);
    CHECK_FALSE(fixture.output.region);
    CHECK(fixture.output.images.empty());
    CHECK(fixture.output.readingGeneration == generation);
    CHECK(fixture.output.selectionRevision == fixture.controller.selectionRevision());
    CHECK(fixture.output.frameStamp.captureEpoch == fixture.capture.streamEpoch());
    CHECK(fixture.output.frameStamp.displayId == StreamedDisplay);
    CHECK(desktopStubs().detectorCall().calls == 2);
    CHECK(fixture.worker.consumedFrameSequence() == 2);

    // Repeated presentation ticks neither renew the deadline nor keep
    // reprocessing the unchanged, suppressed capture.
    (void)fixture.tick();
    fixture.worker.pump();
    CHECK_FALSE(fixture.fetch());
    fixture.setFace({530, 250, 100});
    (void)fixture.advance(3);
    CHECK(desktopStubs().detectorCall().calls == 3);
    CHECK(fixture.controller.readingState(1)->searching);
}

TEST_CASE("A held crop reaches the interface before capture silence suppresses its reading")
{
    ControllerFixture fixture;
    fixture.select();
    fixture.setFace({520, 250, 100});
    fixture.submit(fixture.frame(1));
    REQUIRE(fixture.fetch());
    REQUIRE(fixture.output.region);
    const RegionOfInterest accepted = *fixture.output.region;
    REQUIRE(accepted.toPixels(1000, 500) != InitialRegion.toPixels(1000, 500));
    CHECK(fixture.region == InitialRegion);

    desktopStubs().sessionDetection = {};
    desktopStubs().faces.clear();
    SECTION("successful absence")
    {
        desktopStubs().detectionStatus = FaceDetectionStatus::Completed;
    }
    SECTION("native failure")
    {
        desktopStubs().detectionStatus = FaceDetectionStatus::Failed;
    }
    fixture.submit(fixture.frame(2, {200, 100, 50}));
    REQUIRE(fixture.fetch());
    REQUIRE(fixture.output.region);
    checkSameCrop(*fixture.output.region, accepted, 1000, 500);
    const auto held = fixture.tick();
    REQUIRE(held.applyRegion);
    checkSameCrop(*held.applyRegion, accepted, 1000, 500);
    CHECK_FALSE(held.lostLock);
    CHECK(fixture.controller.contains(1));

    // A control revision must restore the anchor paired with that crop,
    // without re-detecting the held pixels or renewing their evidence.
    fixture.controller.invalidate();
    (void)fixture.tick();
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    REQUIRE(fixture.output.region);
    checkSameCrop(*fixture.output.region, accepted, 1000, 500);
    const auto remapped = fixture.tick();
    REQUIRE(remapped.applyRegion);
    checkSameCrop(*remapped.applyRegion, accepted, 1000, 500);

    // No new source image arrives, so the controller must expire the held
    // evidence itself while preserving the actual worker crop.
    fixture.clock += 1.0;
    const auto searching = fixture.tick();
    CHECK_FALSE(searching.lostLock);
    CHECK_FALSE(searching.applyRegion);
    REQUIRE(fixture.region);
    checkSameCrop(*fixture.region, accepted, 1000, 500);
    CHECK(fixture.controller.contains(1));
    REQUIRE(fixture.controller.readingState(1));
    CHECK(fixture.controller.readingState(1)->searching);
    CHECK(fixture.attach.isAttached(1));
    CHECK(desktopStubs().detectorCall().calls == 2);

    fixture.setFace({530, 250, 100});
    (void)fixture.advance(3);
    CHECK(desktopStubs().detectorCall().calls == 3);
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.suppressed);
    CHECK_FALSE(fixture.output.region);
    CHECK(fixture.output.images.empty());
}

TEST_CASE("An unsupported native detector returns to the ordinary attachment immediately")
{
    ControllerFixture fixture;
    fixture.select();
    REQUIRE(fixture.advance(1).applyRegion);
    const auto last = fixture.region;
    desktopStubs().sessionDetection = {};
    desktopStubs().detectionStatus = FaceDetectionStatus::Unsupported;
    const auto unsupported = fixture.advance(2);
    REQUIRE(unsupported.lostLock);
    CHECK(*unsupported.lostLock == 1u);
    CHECK(fixture.attach.isAttached(1));
    CHECK(fixture.region == last);
    CHECK_FALSE(fixture.controller.contains(1));
}

TEST_CASE("Retirement preserves a face crop whose accepted update the interface has not consumed")
{
    ControllerFixture fixture;
    fixture.select();
    fixture.setFace({520, 250, 100});
    // Publish real scope output without letting the controller consume the
    // matching accepted update. Retirement will replace that latest-value slot.
    fixture.submit(fixture.frame(1));
    REQUIRE(fixture.fetch());
    REQUIRE(fixture.output.region);
    const RegionOfInterest accepted = *fixture.output.region;
    REQUIRE(accepted.toPixels(1000, 500) != InitialRegion.toPixels(1000, 500));
    CHECK(fixture.region == InitialRegion);

    desktopStubs().sessionDetection = {};
    desktopStubs().faces.clear();
    desktopStubs().detectionStatus = FaceDetectionStatus::Unsupported;
    fixture.submit(fixture.frame(2, {200, 100, 50}));
    REQUIRE(fixture.fetch());
    REQUIRE(fixture.output.region);
    checkSameCrop(*fixture.output.region, accepted, 1000, 500);
    REQUIRE(fixture.output.images.contains("org.sidescopes.histogram"));
    CHECK_FALSE(fixture.output.images.at("org.sidescopes.histogram").rgba.empty());

    const auto retired = fixture.tick();
    REQUIRE(retired.lostLock);
    CHECK(*retired.lostLock == 1u);
    REQUIRE(retired.applyRegion);
    checkSameCrop(*retired.applyRegion, accepted, 1000, 500);
    CHECK_FALSE(fixture.controller.contains(1));
    CHECK(fixture.attach.isAttached(1));

    const int detections = desktopStubs().detectorCall().calls;
    fixture.setFace({530, 250, 100});
    (void)fixture.advance(4);
    CHECK(desktopStubs().detectorCall().calls == detections);
    REQUIRE(fixture.fetch());
    REQUIRE(fixture.output.region);
    checkSameCrop(*fixture.output.region, accepted, 1000, 500);
}

TEST_CASE("Searching preserves the crop from an unconsumed accepted update", "[face-loss-reading]")
{
    ControllerFixture fixture;
    fixture.select();
    fixture.setFace({520, 250, 100});
    fixture.submit(fixture.frame(1));
    REQUIRE(fixture.fetch());
    REQUIRE(fixture.output.region);
    const RegionOfInterest accepted = *fixture.output.region;
    REQUIRE(accepted.toPixels(1000, 500) != InitialRegion.toPixels(1000, 500));
    CHECK(fixture.region == InitialRegion);

    desktopStubs().sessionDetection = {};
    desktopStubs().faces.clear();
    fixture.submit(fixture.frame(2));
    fixture.clock += 1.0;
    fixture.submit(fixture.frame(3));
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.suppressed);
    CHECK_FALSE(fixture.output.region);
    CHECK(fixture.output.images.empty());
    const auto searching = fixture.tick();
    CHECK_FALSE(searching.lostLock);
    REQUIRE(searching.applyRegion);
    checkSameCrop(*searching.applyRegion, accepted, 1000, 500);
    REQUIRE(fixture.controller.readingState(1));
    CHECK(fixture.controller.readingState(1)->searching);
    CHECK(fixture.controller.contains(1));
    CHECK(fixture.attach.isAttached(1));
}

TEST_CASE("A manual crop edit remaps held pixels without a second detection")
{
    ControllerFixture fixture;
    fixture.select();
    (void)fixture.advance(1);
    const RegionOfInterest edited{46, 42, 54, 58};
    SECTION("whole display dimensions")
    {
        fixture.controller.rebindCrop(1, edited, fixture.frameSize);
    }
    SECTION("display dimensions remain authoritative for a narrowed size")
    {
        fixture.controller.rebindCrop(1, edited, {200, 100, 1000, 500});
    }
    fixture.region = edited;
    (void)fixture.tick();
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.region == edited);
    CHECK(desktopStubs().detectorCall().calls == 1);
    fixture.setFace({508, 250, 100});
    const auto moved = fixture.advance(2);
    REQUIRE(moved.applyRegion);
    CHECK_THAT(moved.applyRegion->leftPercent, WithinAbs(46.8, 1e-6));
    CHECK_THAT(moved.applyRegion->rightPercent, WithinAbs(54.8, 1e-6));
    CHECK_THAT(moved.applyRegion->topPercent, WithinAbs(42, 1e-6));
}

TEST_CASE("A same-window replacement selection rejects already completed tracking")
{
    ControllerFixture fixture;
    fixture.select();
    fixture.setFace({530, 250, 100});
    fixture.submit(fixture.frame(1));
    fixture.controller.addLock(1, foreheadLock(), FullWindow);
    const auto result = fixture.tick();
    CHECK_FALSE(result.applyRegion);
    CHECK_FALSE(result.lostLock);
    CHECK(fixture.region == InitialRegion);
    CHECK_FALSE(fixture.fetch());
}

TEST_CASE("A selection changed inside detection cannot publish its old crop")
{
    ControllerFixture fixture;
    fixture.select();
    const RegionOfInterest edited{46, 42, 54, 58};
    desktopStubs().beforeDetection = [&] { fixture.controller.rebindCrop(1, edited, fixture.frameSize); };
    fixture.submit(fixture.frame(1));
    desktopStubs().beforeDetection = {};
    fixture.region = edited;
    const auto stale = fixture.tick();
    CHECK_FALSE(stale.applyRegion);
    CHECK_FALSE(stale.lostLock);
    CHECK_FALSE(fixture.fetch());
    fixture.worker.pump();
    REQUIRE(fixture.fetch());
    CHECK(fixture.output.region == edited);
    CHECK(desktopStubs().detectorCall().calls == 1);
}

TEST_CASE("A completed result cannot move another active window")
{
    ControllerFixture fixture;
    fixture.select();
    fixture.setFace({530, 250, 100});
    fixture.submit(fixture.frame(1));
    fixture.decision = {};
    fixture.controller.activationChanged();
    const auto stale = fixture.tick();
    CHECK_FALSE(stale.applyRegion);
    CHECK_FALSE(stale.lostLock);
    CHECK(fixture.controller.contains(1));
    CHECK(fixture.region == InitialRegion);
}

TEST_CASE("One face's uncertainty survives focus changes without expiring another face")
{
    ControllerFixture fixture;
    fixture.select();
    (void)fixture.advance(1);
    desktopStubs().sessionDetection = {};
    (void)fixture.advance(2);
    const double later = fixture.clock + 1.0;
    (void)fixture.attach.attach(2, 2, "Viewer", FullWindow, {0, 0, 1000, 500}, InitialRegion);
    fixture.controller.addLock(2, foreheadLock(), FullWindow);
    fixture.decision.activeIdentity = 2;
    fixture.controller.activationChanged();
    fixture.setFace(InitialAnchor);
    REQUIRE(fixture.advance(3).applyRegion);
    CHECK_FALSE(fixture.tick(later).lostLock);
    CHECK(fixture.controller.contains(1));
    CHECK(fixture.controller.contains(2));
    fixture.decision.activeIdentity = 1;
    fixture.controller.activationChanged();
    const auto expired = fixture.tick(later);
    CHECK_FALSE(expired.lostLock);
    REQUIRE(fixture.controller.readingState(1));
    REQUIRE(fixture.controller.readingState(2));
    CHECK(fixture.controller.readingState(1)->searching);
    CHECK_FALSE(fixture.controller.readingState(2)->searching);
    CHECK(fixture.controller.contains(1));
    CHECK(fixture.controller.contains(2));
    CHECK(fixture.attach.isAttached(1));
}

TEST_CASE("Tracking rejects an old capture identity before calling the detector")
{
    ControllerFixture fixture;
    fixture.select();
    auto frame = fixture.frame(1);
    SECTION("capture epoch")
    {
        ++frame.stamp.captureEpoch;
    }
    SECTION("display identity")
    {
        ++frame.stamp.displayId;
    }
    fixture.submit(std::move(frame));
    CHECK(desktopStubs().detectorCall().calls == 0);
    CHECK_FALSE(fixture.fetch());
    REQUIRE(fixture.advance(2).applyRegion);
}

TEST_CASE("A narrowed capture cannot be mistaken for the full tracking source")
{
    ControllerFixture fixture;
    fixture.select();
    auto narrowed = test::makeSolidFrameBuffer(200, 100, Color{50, 50, 50}, 1);
    narrowed.sourceX = 400;
    narrowed.sourceY = 200;
    narrowed.sourceWidth = 1000;
    narrowed.sourceHeight = 500;
    narrowed.stamp = {fixture.capture.streamEpoch(), StreamedDisplay, fixture.clock};
    fixture.submit(std::move(narrowed));
    CHECK(desktopStubs().detectorCall().calls == 0);
    CHECK_FALSE(fixture.fetch());
    REQUIRE(fixture.advance(2).applyRegion);
}

TEST_CASE("The tracking search uses source pixels and the continuation size floor")
{
    ControllerFixture fixture;
    fixture.frameSize = {2000, 1000, 2000, 1000};
    fixture.select(foreheadLock({500, 400, 100}));
    fixture.setFace({500, 400, 100});
    fixture.submit(coordinateFrame(fixture, 1));
    const auto detected = desktopStubs().detectorCall();
    CHECK(detected.calls == 1);
    CHECK(cropOrigin(detected) == std::pair<int, int>{250, 150});
    CHECK(detected.width == 500);
    CHECK(detected.height == 500);
    CHECK_THAT(detected.minimumPixels, WithinAbs(48.0, 1e-6));
    CHECK(detected.frameSequence == 1u);
    CHECK(detected.stamp.displayId == StreamedDisplay);
}

TEST_CASE("Continuation size follows the capture to desktop coordinate ratio", "[face-continuation-size]")
{
    for (const double scale : {1.0, 1.5, 2.0}) {
        CAPTURE(scale);
        ControllerFixture fixture;
        const auto width = static_cast<int>(1000 * scale);
        const auto height = static_cast<int>(500 * scale);
        fixture.frameSize = {width, height, width, height};
        fixture.select();
        (void)fixture.advance(1);
        const auto detected = desktopStubs().detectorCall();
        REQUIRE(detected.calls == 1);
        CHECK_THAT(detected.minimumPixels, WithinAbs(24.0 * scale, 1e-6));
    }
}

TEST_CASE("A tracking search never crosses the attached window boundary")
{
    ControllerFixture fixture;
    fixture.frameSize = {2000, 1000, 2000, 1000};
    fixture.select(foreheadLock({300, 400, 100}), {0, 0, 200, 500});
    fixture.submit(coordinateFrame(fixture, 1));
    const auto detected = desktopStubs().detectorCall();
    CHECK(cropOrigin(detected) == std::pair<int, int>{50, 150});
    CHECK(detected.width == 350);
    CHECK(detected.height == 500);
}

TEST_CASE("A same-size window move carries the face search in display pixels")
{
    ControllerFixture fixture;
    fixture.frameSize = {2000, 1000, 2000, 1000};
    fixture.select(foreheadLock({500, 400, 100}), {100, 0, 400, 500}, AttachWindowRect{0, 0, 400, 500});
    fixture.submit(coordinateFrame(fixture, 1));
    const auto detected = desktopStubs().detectorCall();
    CHECK(cropOrigin(detected) == std::pair<int, int>{450, 150});
    CHECK(detected.width == 500);
    CHECK(detected.height == 500);
}

TEST_CASE("Window motion and missing geometry pause tracking without removing the face selection")
{
    ControllerFixture fixture;
    fixture.select();
    SECTION("a native window gesture")
    {
        fixture.gesture = true;
    }
    SECTION("a minimized or inaccessible window")
    {
        fixture.decision.activeRect.reset();
    }
    SECTION("missing display geometry")
    {
        desktopStubs().displayGeometry.reset();
    }
    (void)fixture.advance(1);
    CHECK(desktopStubs().detectorCall().calls == 0);
    CHECK(fixture.controller.contains(1));
}

TEST_CASE("Minimize and restore animations preserve the face anchor and accepted crop")
{
    ControllerFixture fixture;
    fixture.select();
    REQUIRE(fixture.advance(1).applyRegion);
    const auto accepted = fixture.region;
    const int detections = desktopStubs().detectorCall().calls;
    auto observe = [&](AttachWindowRect rect, bool minimized, double now) {
        AttachedWindowObservation observation;
        observation.identity = 1;
        observation.windowRect = rect;
        observation.minimized = minimized;
        observation.displayId = StreamedDisplay;
        observation.display = {0, 0, 1000, 500};
        fixture.decision = fixture.attach.observe({observation}, 1, now);
        (void)fixture.tick();
        CHECK(fixture.controller.contains(1));
    };
    // First a same-size translation, then a tiny window: neither may
    // translate the saved anchor or cause validation to retire the face.
    observe({300, 200, 1000, 500}, false, 1.0);
    (void)fixture.advance(2);
    observe({850, 430, 80, 60}, false, 1.05);
    (void)fixture.advance(3);
    observe({850, 430, 80, 60}, true, 1.1);
    (void)fixture.advance(4);
    fixture.controller.invalidate();  // picker open/cancel invalidates work
    observe({850, 430, 80, 60}, false, 5.0);
    observe({850, 430, 80, 60}, false, 5.0);
    CHECK_FALSE(fixture.decision.region);
    observe(FullWindow, false, 5.05);
    CHECK_FALSE(fixture.decision.region);
    CHECK(desktopStubs().detectorCall().calls == detections);
    observe(FullWindow, false, 5.3);
    REQUIRE(fixture.decision.region);
    checkSameCrop(*fixture.decision.region, *accepted, 1000, 500);
    const auto resumed = fixture.advance(5);
    REQUIRE(resumed.applyRegion);
    checkSameCrop(*resumed.applyRegion, *accepted, 1000, 500);
    CHECK(fixture.controller.contains(1));
    CHECK(desktopStubs().detectorCall().calls == detections + 1);
}

TEST_CASE("A depth change reaches the detector as native pixels without a content hiding stage")
{
    ControllerFixture fixture;
    fixture.select();
    (void)fixture.advance(1);
    auto frame = test::makeTenBitFrameBuffer(1000, 500, 616, 616, 616, 2);
    frame.stamp = {fixture.capture.streamEpoch(), StreamedDisplay, fixture.clock};
    fixture.submit(std::move(frame));
    REQUIRE(fixture.tick().applyRegion);
    CHECK(desktopStubs().detectorCall().calls == 2);
    CHECK(desktopStubs().detectorCall().format == PixelFormat::Argb2101010);
}

TEST_CASE("Face selections can be removed cleared or pruned")
{
    ControllerFixture fixture;
    fixture.select();
    fixture.controller.addLock(2, foreheadLock());
    SECTION("remove")
    {
        fixture.controller.removeLock(1);
        CHECK(fixture.attach.isAttached(1));
    }
    SECTION("clear")
    {
        fixture.controller.clear();
        CHECK(fixture.attach.isAttached(1));
    }
    SECTION("prune an attachment already removed")
    {
        fixture.attach.detachAll();
        (void)fixture.tick();
    }
    CHECK_FALSE(fixture.controller.contains(1));
    fixture.controller.clear();
    CHECK_FALSE(fixture.controller.locked());
}

TEST_CASE("Delayed interface consumption keeps the worker loss deadline", "[face-loss-clock]")
{
    ControllerFixture fixture;
    fixture.select();
    REQUIRE(fixture.advance(1).applyRegion);
    desktopStubs().sessionDetection = {};
    desktopStubs().faces.clear();
    fixture.submit(fixture.frame(2));
    const double loss = fixture.clock;

    // The latest miss is first consumed near expiry. Reading it must not
    // start another full grace period on the interface thread.
    const auto held = fixture.tick(loss + 0.999);
    REQUIRE(held.applyRegion);
    CHECK_FALSE(held.lostLock);
    REQUIRE(fixture.controller.readingState(1));
    CHECK_FALSE(fixture.controller.readingState(1)->searching);
    const auto expired = fixture.tick(loss + 1.001);
    CHECK_FALSE(expired.lostLock);
    REQUIRE(fixture.controller.readingState(1));
    CHECK(fixture.controller.readingState(1)->searching);
    CHECK(fixture.attach.isAttached(1));
}

TEST_CASE("An overwritten recovery replaces the interface loss episode", "[face-loss-clock]")
{
    ControllerFixture fixture;
    fixture.select();
    REQUIRE(fixture.advance(1).applyRegion);
    desktopStubs().sessionDetection = {};
    desktopStubs().faces.clear();
    (void)fixture.advance(2);
    const double firstLoss = fixture.clock;

    // Both updates complete before the interface next consumes the slot.
    fixture.setFace({508, 250, 100});
    fixture.submit(fixture.frame(3));
    desktopStubs().sessionDetection = {};
    fixture.submit(fixture.frame(4));
    const double secondLoss = fixture.clock;
    const auto held = fixture.tick(firstLoss + 1.001);
    REQUIRE(held.applyRegion);
    CHECK_FALSE(held.lostLock);
    CHECK(fixture.controller.contains(1));
    REQUIRE(fixture.controller.readingState(1));
    CHECK_FALSE(fixture.controller.readingState(1)->searching);
    const auto expired = fixture.tick(secondLoss + 1.001);
    CHECK_FALSE(expired.lostLock);
    REQUIRE(fixture.controller.readingState(1));
    CHECK(fixture.controller.readingState(1)->searching);
    CHECK(fixture.attach.isAttached(1));
}

TEST_CASE("A clipped parent move preserves recent rival recovery protection", "[clipped-parent]")
{
    ControllerFixture fixture;
    bool horizontal = true;
    SECTION("Left display edge")
    {
    }
    SECTION("Top display edge")
    {
        horizontal = false;
    }
    fixture.select();
    const auto setFaces = [](std::vector<FaceAnchor> faces) {
        desktopStubs().sessionDetection = [faces = std::move(faces)](const FrameView& crop, double) {
            FaceDetectionResult result{FaceDetectionStatus::Completed, {}};
            for (const auto& face : faces) {
                result.faces.push_back({static_cast<int>(face.centerX - face.width / 2) - crop.sourceX,
                                        static_cast<int>(face.centerY - face.width / 2) - crop.sourceY,
                                        static_cast<int>(face.width), static_cast<int>(face.width)});
            }
            return result;
        };
    };
    setFaces({InitialAnchor, {horizontal ? 650.0 : 500.0, horizontal ? 250.0 : 400.0, 100}});
    REQUIRE(fixture.advance(1).applyRegion);
    // The real parent moves by -50; its clipped left/top stays at zero.
    fixture.decision.activeRect = AttachWindowRect{horizontal ? -50.0 : 0.0, horizontal ? 0.0 : -50.0, 1000, 500};
    (void)fixture.tick();
    setFaces({});
    REQUIRE(fixture.advance(2).applyRegion);
    const auto held = fixture.region;
    const double loss = fixture.clock;
    // A surviving rival approaches the last selected position. It is one
    // selected width away, but only half a width from the translated rival.
    fixture.clock += 0.08;
    setFaces({{horizontal ? 550.0 : 500.0, horizontal ? 250.0 : 300.0, 100}});
    const auto guarded = fixture.advance(3);
    REQUIRE(guarded.applyRegion);
    REQUIRE(held);
    CHECK(guarded.applyRegion == held);
    const auto expired = fixture.tick(loss + 1.001);
    CHECK_FALSE(expired.lostLock);
    REQUIRE(fixture.controller.readingState(1));
    CHECK(fixture.controller.readingState(1)->searching);
    CHECK(fixture.attach.isAttached(1));
}

TEST_CASE("Unclipped parent motion invalidates an unchanged clipped source", "[clipped-parent]")
{
    ControllerFixture fixture;
    fixture.select(foreheadLock(), {-100, -100, 1200, 700});
    REQUIRE(fixture.advance(1).applyRegion);
    const auto revision = fixture.controller.selectionRevision();
    // Both rectangles cover the entire display, so the clipped command
    // rectangle is identical even though the parent has moved.
    fixture.decision.activeRect = AttachWindowRect{-90, -95, 1200, 700};
    (void)fixture.tick();
    CHECK(fixture.controller.selectionRevision() > revision);
}

}  // namespace sidescopes
