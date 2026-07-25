#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

#include "app/capture_controller.h"
#include "app/cursor_sampler.h"
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

constexpr uint32_t StreamedDisplay = 4;
// No smoothing, so one step reports the sample itself rather than a fraction
// of the way towards it.
constexpr CursorSmoothing Instant{0.0f, 0.0f};

struct SamplerFixture
{
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    CursorSampler sampler{capture, worker};

    SamplerFixture()
    {
        desktopStubs().reset();
        desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 64.0, 64.0};
        source.targets = {makeTarget(StreamedDisplay, "Test display")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(StreamedDisplay);
        REQUIRE(capture.start());
        worker.start();
        mailbox.publish(makeSolidFrameBuffer(64, 64, Color{200, 50, 30}, 1));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (worker.consumedFrameSequence() != 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(worker.consumedFrameSequence() == 1);
    }

    ~SamplerFixture()
    {
        worker.stop();
    }

    SamplerFixture(const SamplerFixture&) = delete;
    SamplerFixture& operator=(const SamplerFixture&) = delete;
};

constexpr AnalysisWorker::FrameSize FrameSize{64, 64};

}  // namespace

TEST_CASE("The readout takes its colour from the capture stream's own frame")
{
    SamplerFixture fix;
    desktopStubs().cursor = DesktopPoint{32.0, 32.0};
    desktopStubs().cursorDisplay = StreamedDisplay;

    const CursorSample sample = fix.sampler.update(FrameSize, Instant, 1.0, 1.0f / 60.0f);

    REQUIRE(sample.vectorscopeColor.has_value());
    REQUIRE(sample.waveformColor.has_value());
    CHECK_THAT(sample.vectorscopeColor->r, WithinAbs(200.0f, 1.0f));
    CHECK_THAT(sample.waveformColor->b, WithinAbs(30.0f, 1.0f));
    // The one-shot screen read is for other displays only; the stream was
    // right here.
    CHECK(desktopStubs().screenSampleRequests == 0);
}

TEST_CASE("A cursor on another display falls back to a throttled screen read")
{
    SamplerFixture fix;
    desktopStubs().cursor = DesktopPoint{500.0, 32.0};
    desktopStubs().cursorDisplay = StreamedDisplay + 1;
    desktopStubs().screenSample = FloatColor{11.0f, 22.0f, 33.0f};

    const CursorSample first = fix.sampler.update(FrameSize, Instant, 1.0, 1.0f / 60.0f);
    REQUIRE(first.vectorscopeColor.has_value());
    CHECK_THAT(first.vectorscopeColor->g, WithinAbs(22.0f, 1e-3f));
    CHECK(desktopStubs().screenSampleRequests == 1);

    // Reading the screen is expensive, so a second frame within the throttle
    // reuses the sample that already landed rather than asking again.
    desktopStubs().cursor = DesktopPoint{501.0, 32.0};
    const CursorSample throttled = fix.sampler.update(FrameSize, Instant, 1.02, 1.0f / 60.0f);
    REQUIRE(throttled.vectorscopeColor.has_value());
    CHECK_THAT(throttled.vectorscopeColor->g, WithinAbs(22.0f, 1e-3f));
    CHECK(desktopStubs().screenSampleRequests == 1);

    // Past the throttle it asks again.
    (void)fix.sampler.update(FrameSize, Instant, 1.2, 1.0f / 60.0f);
    CHECK(desktopStubs().screenSampleRequests == 2);
}

TEST_CASE("A dead capture stream reads the screen instead of a stale frame")
{
    SamplerFixture fix;
    desktopStubs().cursor = DesktopPoint{32.0, 32.0};
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().screenSample = FloatColor{11.0f, 22.0f, 33.0f};
    // A wake or unlock leaves the stream a zombie: the next service marks it
    // dead and the restart begins.
    fix.source.startSucceeds = false;
    fix.capture.markStale();
    fix.capture.service(1.0);
    REQUIRE(fix.capture.dead());

    // The frame in hand is whatever the stream last delivered before it died,
    // so the readout goes to the screen even on the captured display.
    const CursorSample sample = fix.sampler.update(FrameSize, Instant, 1.0, 1.0f / 60.0f);

    REQUIRE(sample.vectorscopeColor.has_value());
    CHECK_THAT(sample.vectorscopeColor->r, WithinAbs(11.0f, 1e-3f));
    CHECK(desktopStubs().screenSampleRequests == 1);
}

TEST_CASE("Only a marker that moves counts as interaction")
{
    // What these samples feed is a marker drawn at the cursor's COLOUR, so the
    // colour is what has to change for a frame to be worth spending. The frame
    // here is one solid colour, which is what sliding across a flat area of a
    // photograph looks like: the pointer travels and the marker does not move.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;

    // The first reading brings a marker into existence.
    desktopStubs().cursor = DesktopPoint{32.0, 32.0};
    CHECK(fix.sampler.update(FrameSize, Instant, 1.0, 1.0f / 60.0f).changed);

    // A pointer sitting still must not keep the application awake.
    CHECK_FALSE(fix.sampler.update(FrameSize, Instant, 1.1, 1.0f / 60.0f).changed);

    // Nor must a pointer travelling across a uniform frame - this is the case
    // that was waking the loop sixty-five times a second for a picture that
    // never changed.
    for (const double x : {40.0, 48.0, 56.0, 20.0}) {
        desktopStubs().cursor = DesktopPoint{x, 32.0};
        CHECK_FALSE(fix.sampler.update(FrameSize, Instant, 1.2, 1.0f / 60.0f).changed);
    }

    // But a colour that really changes under the pointer does wake it.
    fix.mailbox.publish(makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == 2);
    CHECK(fix.sampler.update(FrameSize, Instant, 1.3, 1.0f / 60.0f).changed);
}

TEST_CASE("A marker still easing towards its colour keeps redrawing")
{
    // With smoothing on, the marker travels for several frames after the colour
    // under the pointer changes, and every one of those frames has to be drawn.
    // Waking only on the raw sample would freeze the marker part way.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    constexpr CursorSmoothing Smoothed{200.0f, 200.0f};

    desktopStubs().cursor = DesktopPoint{32.0, 32.0};
    REQUIRE(fix.sampler.update(FrameSize, Smoothed, 1.0, 1.0f / 60.0f).changed);

    // The pointer never moves again; the marker is still on its way.
    int easingFrames = 0;
    for (int i = 0; i < 60; ++i) {
        if (fix.sampler.update(FrameSize, Smoothed, 1.0 + 0.016 * i, 1.0f / 60.0f).changed) {
            ++easingFrames;
        }
    }
    CHECK(easingFrames > 0);

    // And it does settle, rather than creeping towards its target for ever.
    for (int i = 0; i < 400; ++i) {
        (void)fix.sampler.update(FrameSize, Smoothed, 2.0 + 0.016 * i, 1.0f / 60.0f);
    }
    CHECK_FALSE(fix.sampler.update(FrameSize, Smoothed, 12.0, 1.0f / 60.0f).changed);
}

TEST_CASE("Nothing is sampled without a capture stream or a pointer")
{
    desktopStubs().reset();
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    CursorSampler sampler{capture, worker};
    desktopStubs().cursor = DesktopPoint{32.0, 32.0};
    REQUIRE(capture.capturedDisplay() == 0);

    // No display is being watched, so there is no readout to keep alive.
    const CursorSample noCapture = sampler.update(FrameSize, Instant, 1.0, 1.0f / 60.0f);
    CHECK_FALSE(noCapture.changed);
    CHECK_FALSE(noCapture.vectorscopeColor.has_value());
    CHECK(desktopStubs().screenSampleRequests == 0);

    // Nor when the platform cannot say where the pointer is.
    SamplerFixture fix;
    desktopStubs().cursor.reset();
    const CursorSample noCursor = fix.sampler.update(FrameSize, Instant, 1.0, 1.0f / 60.0f);
    CHECK_FALSE(noCursor.changed);
    CHECK_FALSE(noCursor.vectorscopeColor.has_value());
    CHECK(desktopStubs().screenSampleRequests == 0);
}

TEST_CASE("The last cross-display sample is what the pin tool reads")
{
    SamplerFixture fix;
    CHECK_FALSE(fix.sampler.screenSampleColor().has_value());

    desktopStubs().cursor = DesktopPoint{500.0, 32.0};
    desktopStubs().cursorDisplay = StreamedDisplay + 1;
    desktopStubs().screenSample = FloatColor{11.0f, 22.0f, 33.0f};
    (void)fix.sampler.update(FrameSize, Instant, 1.0, 1.0f / 60.0f);

    // The picker's pin poll is handed this each frame, so a colour pinned on
    // another display is the one the readout was already showing.
    REQUIRE(fix.sampler.screenSampleColor().has_value());
    CHECK_THAT(fix.sampler.screenSampleColor()->b, WithinAbs(33.0f, 1e-3f));
}

}  // namespace sidescopes
