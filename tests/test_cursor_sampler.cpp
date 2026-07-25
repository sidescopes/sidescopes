#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

#include "app/capture_controller.h"
#include "app/cursor_sampler.h"
#include "app/frame_pacing.h"
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

constexpr AnalysisWorker::FrameSize FrameSize{64, 64, 64, 64};
// The whole captured display: every point on it carries a marker.
constexpr RegionOfInterest WholeDisplay{0.0, 0.0, 100.0, 100.0};
// The top-left quarter of it, in display pixels [0,32).
constexpr RegionOfInterest Quadrant{0.0, 0.0, 50.0, 50.0};
// The region a narrowed capture was narrowed to: display pixels [24,40).
constexpr RegionOfInterest CroppedRegion{37.5, 37.5, 62.5, 62.5};
// The same display captured as a 16x16 crop at 24,24: what the stream delivers
// once the analysis region is small enough to narrow to.
constexpr AnalysisWorker::FrameSize NarrowedFrameSize{16, 16, 64, 64};

// A frame carrying only [24,40) of a 64x64 display, in one flat colour.
FrameBuffer makeNarrowedFrameBuffer(Color color, uint64_t sequence)
{
    FrameBuffer frame = makeSolidFrameBuffer(16, 16, color, sequence);
    frame.sourceX = 24;
    frame.sourceY = 24;
    frame.sourceWidth = 64;
    frame.sourceHeight = 64;

    return frame;
}

}  // namespace

TEST_CASE("The readout takes its colour from the capture stream's own frame")
{
    SamplerFixture fix;
    desktopStubs().cursor = DesktopPoint{32.0, 32.0};
    desktopStubs().cursorDisplay = StreamedDisplay;

    const CursorSample sample = fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.0, 1.0f / 60.0f);

    REQUIRE(sample.vectorscopeColor.has_value());
    REQUIRE(sample.waveformColor.has_value());
    CHECK_THAT(sample.vectorscopeColor->r, WithinAbs(200.0f, 1.0f));
    CHECK_THAT(sample.waveformColor->b, WithinAbs(30.0f, 1.0f));
    // The one-shot screen read is for other displays only; the stream was
    // right here.
    CHECK(desktopStubs().screenSampleRequests == 0);
}

TEST_CASE("A narrowed capture still reads the point the cursor is over")
{
    // With the capture narrowed to the analysis region, the frame is a small
    // window on the display. Scaling the cursor by the FRAME's extents put every
    // point on the display into that little rectangle, so the readout showed one
    // colour wherever the pointer went.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().screenSample = FloatColor{11.0f, 22.0f, 33.0f};
    fix.mailbox.publish(makeNarrowedFrameBuffer(Color{0, 128, 255}, 2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == 2);

    // Inside the crop: the stream's own pixels answer.
    desktopStubs().cursor = DesktopPoint{32.0, 32.0};
    const CursorSample inside = fix.sampler.update(NarrowedFrameSize, CroppedRegion, Instant, 1.0, 1.0f / 60.0f);
    REQUIRE(inside.vectorscopeColor.has_value());
    CHECK_THAT(inside.vectorscopeColor->b, WithinAbs(255.0f, 1.0f));
    CHECK(desktopStubs().screenSampleRequests == 0);

    // Outside it the capture simply holds nothing, so the readout goes to the
    // one-shot screen sampler rather than reporting a pixel from inside the
    // region.
    desktopStubs().cursor = DesktopPoint{4.0, 4.0};
    const CursorSample outside =
        fix.sampler.update(NarrowedFrameSize, CroppedRegion, Instant, 1.0 + ReadoutSampleSeconds + 0.001, 1.0f / 60.0f);
    REQUIRE(outside.readoutColor.has_value());
    CHECK_THAT(outside.readoutColor->g, WithinAbs(22.0f, 1e-3f));
    CHECK(desktopStubs().screenSampleRequests == 1);
}

TEST_CASE("A cursor on another display falls back to a throttled screen read")
{
    SamplerFixture fix;
    desktopStubs().cursor = DesktopPoint{500.0, 32.0};
    desktopStubs().cursorDisplay = StreamedDisplay + 1;
    desktopStubs().screenSample = FloatColor{11.0f, 22.0f, 33.0f};

    const CursorSample first = fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.0, 1.0f / 60.0f);
    REQUIRE(first.readoutColor.has_value());
    CHECK_THAT(first.readoutColor->g, WithinAbs(22.0f, 1e-3f));
    CHECK(desktopStubs().screenSampleRequests == 1);

    // Reading the screen is expensive, so a second frame within the throttle
    // reuses the sample that already landed rather than asking again.
    desktopStubs().cursor = DesktopPoint{501.0, 32.0};
    const CursorSample throttled = fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.02, 1.0f / 60.0f);
    REQUIRE(throttled.readoutColor.has_value());
    CHECK_THAT(throttled.readoutColor->g, WithinAbs(22.0f, 1e-3f));
    CHECK(desktopStubs().screenSampleRequests == 1);

    // Past the throttle it asks again.
    (void)fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.2, 1.0f / 60.0f);
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
    const CursorSample sample = fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.0, 1.0f / 60.0f);

    REQUIRE(sample.readoutColor.has_value());
    CHECK_THAT(sample.readoutColor->r, WithinAbs(11.0f, 1e-3f));
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
    CHECK(fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.0, 1.0f / 60.0f).changed);

    // A pointer sitting still must not keep the application awake.
    CHECK_FALSE(fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.1, 1.0f / 60.0f).changed);

    // Nor must a pointer travelling across a uniform frame - this is the case
    // that was waking the loop sixty-five times a second for a picture that
    // never changed.
    for (const double x : {40.0, 48.0, 56.0, 20.0}) {
        desktopStubs().cursor = DesktopPoint{x, 32.0};
        CHECK_FALSE(fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.2, 1.0f / 60.0f).changed);
    }

    // But a colour that really changes under the pointer does wake it.
    fix.mailbox.publish(makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == 2);
    CHECK(fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.3, 1.0f / 60.0f).changed);
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
    REQUIRE(fix.sampler.update(FrameSize, WholeDisplay, Smoothed, 1.0, 1.0f / 60.0f).changed);

    // The pointer never moves again; the marker is still on its way.
    int easingFrames = 0;
    for (int i = 0; i < 60; ++i) {
        if (fix.sampler.update(FrameSize, WholeDisplay, Smoothed, 1.0 + 0.016 * i, 1.0f / 60.0f).changed) {
            ++easingFrames;
        }
    }
    CHECK(easingFrames > 0);

    // And it does settle, rather than creeping towards its target for ever.
    for (int i = 0; i < 400; ++i) {
        (void)fix.sampler.update(FrameSize, WholeDisplay, Smoothed, 2.0 + 0.016 * i, 1.0f / 60.0f);
    }
    CHECK_FALSE(fix.sampler.update(FrameSize, WholeDisplay, Smoothed, 12.0, 1.0f / 60.0f).changed);
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
    const CursorSample noCapture = sampler.update(FrameSize, WholeDisplay, Instant, 1.0, 1.0f / 60.0f);
    CHECK_FALSE(noCapture.changed);
    CHECK_FALSE(noCapture.vectorscopeColor.has_value());
    CHECK_FALSE(noCapture.readoutColor.has_value());
    CHECK(desktopStubs().screenSampleRequests == 0);

    // Nor when the platform cannot say where the pointer is.
    SamplerFixture fix;
    desktopStubs().cursor.reset();
    const CursorSample noCursor = fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.0, 1.0f / 60.0f);
    CHECK_FALSE(noCursor.changed);
    CHECK_FALSE(noCursor.readoutColor.has_value());
    CHECK(desktopStubs().screenSampleRequests == 0);
}

TEST_CASE("A marker stands only for a colour inside the region")
{
    // A marker says "this colour sits here in the distribution you are looking
    // at", so a sample from outside the region cannot support one - and outside
    // the region is where most of a session's pointer travel happens, over
    // window chrome and editor sliders. The readout keeps following it, because
    // a swatch and a hex code claim nothing about the distribution.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().screenSample = FloatColor{11.0f, 22.0f, 33.0f};

    desktopStubs().cursor = DesktopPoint{8.0, 8.0};
    const CursorSample inside = fix.sampler.update(FrameSize, Quadrant, Instant, 1.0, 1.0f / 60.0f);
    REQUIRE(inside.vectorscopeColor.has_value());
    REQUIRE(inside.waveformColor.has_value());
    REQUIRE(inside.readoutColor.has_value());
    CHECK_THAT(inside.vectorscopeColor->r, WithinAbs(200.0f, 1.0f));

    desktopStubs().cursor = DesktopPoint{48.0, 48.0};
    const CursorSample outside = fix.sampler.update(FrameSize, Quadrant, Instant, 1.1, 1.0f / 60.0f);
    CHECK_FALSE(outside.vectorscopeColor.has_value());
    CHECK_FALSE(outside.waveformColor.has_value());
    REQUIRE(outside.readoutColor.has_value());
    CHECK_THAT(outside.readoutColor->r, WithinAbs(200.0f, 1.0f));

    // Leaving is itself a change - the marker has to be taken off the trace -
    // but travelling on outside the region is not. That second part is the
    // baseline the application idles at while the user works in the editor
    // beside it: the pointer moves all day and no marker is being asked for.
    CHECK(outside.changed);
    desktopStubs().cursor = DesktopPoint{60.0, 40.0};
    CHECK_FALSE(fix.sampler.update(FrameSize, Quadrant, Instant, 1.2, 1.0f / 60.0f).changed);
}

TEST_CASE("A marker that comes back appears where the pointer is")
{
    // Leaving the region ends the marker rather than freezing it, so the
    // smoothing has nothing to ease from when the pointer returns: the marker
    // must appear at the colour under it, not sweep across the trace from the
    // colour it was showing when it left.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    constexpr CursorSmoothing Smoothed{200.0f, 200.0f};

    desktopStubs().cursor = DesktopPoint{8.0, 8.0};
    for (int step = 0; step < 200; ++step) {
        (void)fix.sampler.update(FrameSize, Quadrant, Smoothed, 1.0 + 0.016 * step, 1.0f / 60.0f);
    }
    desktopStubs().cursor = DesktopPoint{48.0, 48.0};
    REQUIRE_FALSE(fix.sampler.update(FrameSize, Quadrant, Smoothed, 5.0, 1.0f / 60.0f).vectorscopeColor.has_value());

    // A different colour under the pointer on the way back in.
    fix.mailbox.publish(makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == 2);
    desktopStubs().cursor = DesktopPoint{8.0, 8.0};
    const CursorSample back = fix.sampler.update(FrameSize, Quadrant, Smoothed, 5.1, 1.0f / 60.0f);

    REQUIRE(back.vectorscopeColor.has_value());
    CHECK_THAT(back.vectorscopeColor->r, WithinAbs(20.0f, 1.0f));
    CHECK_THAT(back.vectorscopeColor->g, WithinAbs(200.0f, 1.0f));
}

TEST_CASE("A marker takes a new colour at its own interval")
{
    // Sampled every frame, a marker chases thirty colours a second across a
    // photograph and reads as jumping about. It takes a new one twelve times a
    // second instead, and travels towards that one in between.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{8.0, 8.0};

    const CursorSample first = fix.sampler.update(FrameSize, Quadrant, Instant, 1.0, 1.0f / 60.0f);
    REQUIRE(first.vectorscopeColor.has_value());
    CHECK_THAT(first.vectorscopeColor->r, WithinAbs(200.0f, 1.0f));

    // A different colour under the pointer, well inside the interval: the
    // marker is still travelling towards the one it took.
    fix.mailbox.publish(makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == 2);

    const CursorSample within =
        fix.sampler.update(FrameSize, Quadrant, Instant, 1.0 + MarkerSampleSeconds / 2.0, 1.0f / 60.0f);
    REQUIRE(within.vectorscopeColor.has_value());
    CHECK_THAT(within.vectorscopeColor->r, WithinAbs(200.0f, 1.0f));
    CHECK_FALSE(within.changed);

    // Past the interval it takes the new one.
    const CursorSample after =
        fix.sampler.update(FrameSize, Quadrant, Instant, 1.0 + MarkerSampleSeconds + 0.001, 1.0f / 60.0f);
    REQUIRE(after.vectorscopeColor.has_value());
    CHECK_THAT(after.vectorscopeColor->g, WithinAbs(200.0f, 1.0f));
    CHECK(after.changed);

    // The readout keeps a slower interval of its own - it has the same
    // complaint and less reason to move at all - so it is still showing the
    // colour it took, and picks the new one up at its own turn.
    REQUIRE(after.readoutColor.has_value());
    CHECK_THAT(after.readoutColor->r, WithinAbs(200.0f, 1.0f));
    CHECK(ReadoutSampleSeconds > MarkerSampleSeconds);
    const CursorSample readout =
        fix.sampler.update(FrameSize, Quadrant, Instant, 1.0 + ReadoutSampleSeconds + 0.001, 1.0f / 60.0f);
    REQUIRE(readout.readoutColor.has_value());
    CHECK_THAT(readout.readoutColor->g, WithinAbs(200.0f, 1.0f));
}

TEST_CASE("The loop's readout cadence matches the rate the pointer is probed at")
{
    // A frame drawn between two probes redraws a swatch that cannot have
    // changed; a probe taken between two frames is a reading nothing shows.
    CHECK(ReadoutRedrawSeconds == ReadoutSampleSeconds);
}

TEST_CASE("Markers can be made to follow the pointer anywhere")
{
    // Whether a marker stands only for a colour inside the region is an open
    // decision - MarkersFollowRegion is the whole of it - so both readings are
    // exercised rather than only the one in force.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{48.0, 48.0};

    fix.sampler.setMarkersFollowRegion(true);
    CHECK_FALSE(fix.sampler.update(FrameSize, Quadrant, Instant, 1.0, 1.0f / 60.0f).vectorscopeColor.has_value());

    fix.sampler.setMarkersFollowRegion(false);
    const CursorSample global = fix.sampler.update(FrameSize, Quadrant, Instant, 2.0, 1.0f / 60.0f);
    REQUIRE(global.vectorscopeColor.has_value());
    CHECK_THAT(global.vectorscopeColor->r, WithinAbs(200.0f, 1.0f));
    CHECK(global.changed);
}

TEST_CASE("The readout reports its own movement")
{
    // The readout carries no motion - a swatch and a number look the same at a
    // third of the frame rate - so the host follows it at its own pace, and
    // that needs a signal separate from the marker's.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{48.0, 48.0};

    // The first sample brings the readout into existence.
    CHECK(fix.sampler.update(FrameSize, Quadrant, Instant, 1.0, 1.0f / 60.0f).readoutChanged);
    CHECK_FALSE(fix.sampler.update(FrameSize, Quadrant, Instant, 1.1, 1.0f / 60.0f).readoutChanged);

    fix.mailbox.publish(makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == 2);

    const CursorSample moved = fix.sampler.update(FrameSize, Quadrant, Instant, 1.2, 1.0f / 60.0f);
    CHECK(moved.readoutChanged);
    // Outside the region, so the marker that used to carry this frame is not
    // involved at all.
    CHECK_FALSE(moved.changed);
}

TEST_CASE("The last cross-display sample is what the pin tool reads")
{
    SamplerFixture fix;
    CHECK_FALSE(fix.sampler.screenSampleColor().has_value());

    desktopStubs().cursor = DesktopPoint{500.0, 32.0};
    desktopStubs().cursorDisplay = StreamedDisplay + 1;
    desktopStubs().screenSample = FloatColor{11.0f, 22.0f, 33.0f};
    (void)fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.0, 1.0f / 60.0f);

    // The picker's pin poll is handed this each frame, so a colour pinned on
    // another display is the one the readout was already showing.
    REQUIRE(fix.sampler.screenSampleColor().has_value());
    CHECK_THAT(fix.sampler.screenSampleColor()->b, WithinAbs(33.0f, 1e-3f));
}

}  // namespace sidescopes
