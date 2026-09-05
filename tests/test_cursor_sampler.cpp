#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

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

// The shipped marker smoothing: 75 ms on the vectorscope, 100 on the waveform
// (the defaults in preferences.h). What the two tests about how a marker
// travels are measured against, since the travelling is the smoothing's doing.
constexpr CursorSmoothing Shipped{75.0f, 100.0f};
// One frame at the cadence the loop redraws at while something is moving.
constexpr float MovingFrame = 1.0f / 30.0f;

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

// A horizontal grey ramp across the display: column x carries x * 255 / 63 in
// every channel, so the colour under the pointer is a known function of where
// the pointer is - a photograph's gradient, with nothing else in it.
FrameBuffer makeRampFrameBuffer(uint64_t sequence)
{
    FrameBuffer frame = makeSolidFrameBuffer(64, 64, Color{0, 0, 0}, sequence);
    for (int py = 0; py < 64; ++py) {
        for (int px = 0; px < 64; ++px) {
            const auto level = static_cast<uint8_t>(px * 255 / 63);
            uint8_t* pixel =
                frame.data.data() + static_cast<std::size_t>(py) * frame.strideBytes + static_cast<std::size_t>(px) * 4;
            pixel[0] = level;
            pixel[1] = level;
            pixel[2] = level;
        }
    }

    return frame;
}

// Publishes @p frame and waits for the worker to take it, so the next sample
// reads it rather than the one before.
void publishAndAwait(SamplerFixture& fix, FrameBuffer frame)
{
    const uint64_t sequence = frame.sequence;
    fix.mailbox.publish(std::move(frame));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.worker.consumedFrameSequence() != sequence && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fix.worker.consumedFrameSequence() == sequence);
}

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
    publishAndAwait(fix, makeNarrowedFrameBuffer(Color{0, 128, 255}, 2));

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
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));
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

TEST_CASE("A region-scoped marker stands only for a colour inside it")
{
    // The other reading of MarkersFollowRegion: a marker as an assertion about
    // the distribution on screen, which a sample from window chrome cannot
    // support. The readout keeps following the pointer either way, because a
    // swatch and a hex code claim nothing about a distribution.
    SamplerFixture fix;
    fix.sampler.setMarkersFollowRegion(true);
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
    // A marker that stops being drawn - the pointer off screen, or outside the
    // region while markers are scoped to it - ends rather than freezes, so the
    // smoothing has nothing to ease from when it returns: it must appear at the
    // colour under the pointer, not sweep across the trace from the colour it
    // was showing when it left.
    SamplerFixture fix;
    fix.sampler.setMarkersFollowRegion(true);
    desktopStubs().cursorDisplay = StreamedDisplay;
    constexpr CursorSmoothing Smoothed{200.0f, 200.0f};

    desktopStubs().cursor = DesktopPoint{8.0, 8.0};
    for (int step = 0; step < 200; ++step) {
        (void)fix.sampler.update(FrameSize, Quadrant, Smoothed, 1.0 + 0.016 * step, 1.0f / 60.0f);
    }
    desktopStubs().cursor = DesktopPoint{48.0, 48.0};
    REQUIRE_FALSE(fix.sampler.update(FrameSize, Quadrant, Smoothed, 5.0, 1.0f / 60.0f).vectorscopeColor.has_value());

    // A different colour under the pointer on the way back in.
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));
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
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));

    const CursorSample within =
        fix.sampler.update(FrameSize, Quadrant, Instant, 1.0 + MarkerSampleSeconds / 2.0, 1.0f / 60.0f);
    REQUIRE(within.vectorscopeColor.has_value());
    CHECK_THAT(within.vectorscopeColor->r, WithinAbs(200.0f, 1.0f));
    CHECK_FALSE(within.changed);

    // Past the interval it takes the new one and sets off towards it: a moment
    // after the sample lands it has barely left the old colour, and it arrives
    // an interval later, as the sample after that lands.
    const CursorSample after =
        fix.sampler.update(FrameSize, Quadrant, Instant, 1.0 + MarkerSampleSeconds + 0.001, 1.0f / 60.0f);
    REQUIRE(after.vectorscopeColor.has_value());
    CHECK_THAT(after.vectorscopeColor->g, WithinAbs(50.0f, 5.0f));
    CHECK(after.changed);

    const CursorSample arrived =
        fix.sampler.update(FrameSize, Quadrant, Instant, 1.0 + 2.0 * MarkerSampleSeconds, 1.0f / 60.0f);
    REQUIRE(arrived.vectorscopeColor.has_value());
    CHECK_THAT(arrived.vectorscopeColor->g, WithinAbs(200.0f, 3.0f));

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

TEST_CASE("A marker does not spend a frame on movement below a pixel")
{
    // The colour under the pointer is a 3x3 average, so one pixel of that
    // neighbourhood changing by a single code moves it by a ninth of one -
    // around a tenth of a pane pixel once a scope has mapped 0-255 across its
    // width. That must not wake the loop to the moving cadence, or a photograph
    // with the faintest noise in it keeps the application at thirty frames a
    // second for nothing.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{32.0, 32.0};
    REQUIRE(fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.0, 1.0f / 60.0f).changed);

    FrameBuffer nudged = makeSolidFrameBuffer(64, 64, Color{200, 50, 30}, 2);
    // One pixel of the nine, one code brighter: its green byte, which is the
    // second of the four a BGRA pixel carries.
    constexpr std::size_t Neighbour = 31;
    const std::size_t green = Neighbour * static_cast<std::size_t>(nudged.strideBytes) + Neighbour * 4 + 1;
    nudged.data[green] = 51;
    publishAndAwait(fix, std::move(nudged));

    // The reading really did change - a ninth of a code - and is still not
    // worth a frame.
    const CursorSample nudge = fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.2, 1.0f / 60.0f);
    REQUIRE(nudge.vectorscopeColor.has_value());
    CHECK_THAT(nudge.vectorscopeColor->g, WithinAbs(50.0f, 0.2f));
    CHECK_FALSE(nudge.changed);

    // A whole code across the neighbourhood is a pane pixel, and does count.
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{200, 52, 30}, 3));
    CHECK(fix.sampler.update(FrameSize, WholeDisplay, Instant, 1.4, 1.0f / 60.0f).changed);
}

TEST_CASE("A marker crosses the trace at an even speed between samples")
{
    // Sampling and animation are separate rates. Easing straight onto a target
    // refreshed twelve times a second spends most of each step in the first
    // frame after it and crawls through the rest, which reads as a stutter;
    // the target glides between samples instead, so the marker covers a
    // quarter of the way in a quarter of the interval. Smoothing is off here,
    // so what is measured is the glide and not the ease on top of it.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{8.0, 8.0};

    REQUIRE(fix.sampler.update(FrameSize, Quadrant, Instant, 1.0, 1.0f / 60.0f).vectorscopeColor.has_value());
    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));

    // The sample that takes the new colour, and the glide it starts.
    constexpr double Sampled = 1.0 + MarkerSampleSeconds;
    REQUIRE(fix.sampler.update(FrameSize, Quadrant, Instant, Sampled, 1.0f / 60.0f).vectorscopeColor.has_value());
    for (const double covered : {0.25, 0.5, 0.75}) {
        const CursorSample step =
            fix.sampler.update(FrameSize, Quadrant, Instant, Sampled + MarkerSampleSeconds * covered, 1.0f / 60.0f);
        REQUIRE(step.vectorscopeColor.has_value());
        CHECK_THAT(step.vectorscopeColor->g, WithinAbs(static_cast<float>(50.0 + 150.0 * covered), 2.0f));
        // Every one of those frames is movement, so the loop keeps drawing at
        // the moving cadence for the whole of the glide.
        CHECK(step.changed);
    }

    // And it stops when it arrives: a glide that has run out with the colour
    // under the pointer unchanged asks for no more frames.
    for (int frame = 1; frame <= 30; ++frame) {
        (void)fix.sampler.update(FrameSize, Quadrant, Instant, Sampled + 1.0 + 0.033 * frame, 1.0f / 60.0f);
    }
    CHECK_FALSE(fix.sampler.update(FrameSize, Quadrant, Instant, Sampled + 2.0, 1.0f / 60.0f).changed);
}

TEST_CASE("The loop's readout cadence matches the rate the pointer is probed at")
{
    // A frame drawn between two probes redraws a swatch that cannot have
    // changed; a probe taken between two frames is a reading nothing shows.
    CHECK(ReadoutRedrawSeconds == ReadoutSampleSeconds);
}

TEST_CASE("Markers follow the pointer wherever it goes")
{
    // The region and the pointer are separate inputs: the region decides what
    // the traces are built from, and a marker is a live probe of what is under
    // the pointer - worth having with no region drawn at all.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{48.0, 48.0};

    const CursorSample outside = fix.sampler.update(FrameSize, Quadrant, Instant, 1.0, 1.0f / 60.0f);
    REQUIRE(outside.vectorscopeColor.has_value());
    CHECK_THAT(outside.vectorscopeColor->r, WithinAbs(200.0f, 1.0f));
    CHECK(outside.changed);

    // And the scope is one predicate, so the other reading of it still works.
    fix.sampler.setMarkersFollowRegion(true);
    CHECK_FALSE(fix.sampler.update(FrameSize, Quadrant, Instant, 2.0, 1.0f / 60.0f).vectorscopeColor.has_value());
}

TEST_CASE("The readout reports its own movement")
{
    // The readout carries no motion - a swatch and a number look the same at a
    // third of the frame rate - so the host follows it at its own pace, and
    // that needs a signal separate from the marker's.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{48.0, 48.0};

    // The first sample brings both into existence.
    CHECK(fix.sampler.update(FrameSize, Quadrant, Instant, 1.0, 1.0f / 60.0f).readoutChanged);

    publishAndAwait(fix, makeSolidFrameBuffer(64, 64, Color{20, 200, 90}, 2));

    // The readout's interval is the slower of the two, so the marker takes the
    // new colour first and reports its own move while the readout waits its
    // turn - which is what the two separate signals are for.
    const CursorSample marker =
        fix.sampler.update(FrameSize, Quadrant, Instant, 1.0 + MarkerSampleSeconds + 0.001, 1.0f / 60.0f);
    CHECK(marker.changed);
    CHECK_FALSE(marker.readoutChanged);
    CHECK(fix.sampler.update(FrameSize, Quadrant, Instant, 1.0 + ReadoutSampleSeconds + 0.001, 1.0f / 60.0f)
              .readoutChanged);
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

TEST_CASE("A marker reaches a colour the pointer was moved to promptly")
{
    // The regression this guards: the reading a pointer was deliberately moved
    // to arrive at kept the pointer waiting, because a fixed time constant
    // needs a further one for every e-fold of distance. Measured end to end
    // through the sampler - the wait for the next sample, the glide across it,
    // and the smoothing on top - at the cadence the loop redraws at, and with
    // the smoothing the application ships.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{4.0, 32.0};
    publishAndAwait(fix, makeRampFrameBuffer(2));

    double now = 1.0;
    for (int frame = 0; frame < 60; ++frame, now += MovingFrame) {
        (void)fix.sampler.update(FrameSize, WholeDisplay, Shipped, now, MovingFrame);
    }
    // Right across the ramp, in one move, and held.
    desktopStubs().cursor = DesktopPoint{59.0, 32.0};
    const double arrived = now;
    // The three ramp columns the 3x3 neighbourhood there averages, in the
    // frame's own integer levels: what the marker really converges on.
    const int neighbourhood = (58 * 255 / 63) + (59 * 255 / 63) + (60 * 255 / 63);
    const auto target = static_cast<float>(neighbourhood) / 3.0f;
    double reached = -1.0;
    for (int frame = 0; frame < 60 && reached < 0.0; ++frame, now += MovingFrame) {
        const CursorSample sample = fix.sampler.update(FrameSize, WholeDisplay, Shipped, now, MovingFrame);
        REQUIRE(sample.waveformColor.has_value());
        if (std::abs(sample.waveformColor->g - target) <= 1.0f) {
            reached = now - arrived;
        }
    }

    REQUIRE(reached >= 0.0);
    // A quarter of a second, over a distance of 230 codes. The same journey
    // took over half of one before the smoothing took the distance into
    // account, and the waveform is the slower of the two markers.
    CHECK(reached <= 0.25);
}

TEST_CASE("A marker follows a moving pointer at an even speed")
{
    // The other side of the same knob, and the property that must survive it:
    // a marker crossing a photograph moves by roughly the same amount every
    // frame. Easing onto a target refreshed twelve times a second used to
    // spend most of each step in the first frame after it and crawl through
    // the rest, which is what read as a stutter; the glide between samples is
    // what fixed it, and a smoothing that closes distance faster must not
    // uncover it again.
    SamplerFixture fix;
    desktopStubs().cursorDisplay = StreamedDisplay;
    desktopStubs().cursor = DesktopPoint{4.0, 32.0};
    publishAndAwait(fix, makeRampFrameBuffer(2));

    double now = 1.0;
    for (int frame = 0; frame < 30; ++frame, now += MovingFrame) {
        (void)fix.sampler.update(FrameSize, WholeDisplay, Shipped, now, MovingFrame);
    }
    // Two pixels a frame across the ramp: about eight codes a frame, the pace
    // of a brisk pointer crossing a picture. The first few frames are lead-in
    // and not measured - a marker sampled twelve times a second cannot move
    // before its next sample lands, which is a property of the sampling rate
    // rather than of how it travels once it is under way.
    constexpr int LeadIn = 6;
    std::vector<float> steps;
    float previous = -1.0f;
    for (int frame = 0; frame < LeadIn + 20; ++frame, now += MovingFrame) {
        desktopStubs().cursor = DesktopPoint{4.0 + (2 * frame), 32.0};
        const CursorSample sample = fix.sampler.update(FrameSize, WholeDisplay, Shipped, now, MovingFrame);
        REQUIRE(sample.vectorscopeColor.has_value());
        if (frame >= LeadIn && previous >= 0.0f) {
            steps.push_back(sample.vectorscopeColor->g - previous);
        }
        previous = sample.vectorscopeColor->g;
    }

    REQUIRE(steps.size() > 15);
    const float mean = std::accumulate(steps.begin(), steps.end(), 0.0f) / static_cast<float>(steps.size());
    REQUIRE(mean > 4.0f);  // the sweep really is moving, so the ratios below mean something
    for (const float step : steps) {
        // No frame off the average by half. Without the glide a third of them
        // are, and the fastest is half again the average while the slowest
        // covers a fifth of it.
        CHECK(step > mean * 0.5f);
        CHECK(step < mean * 1.5f);
    }
}

}  // namespace sidescopes
