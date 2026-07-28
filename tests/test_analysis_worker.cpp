#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "app/adaptive_detail.h"
#include "core/analysis_worker.h"
#include "core/scopes/sampling.h"
#include "core/scopes/waveform.h"
#include "scope_image.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

namespace {

using namespace std::chrono_literals;

// The built-in module scope ids the worker keys its settings and output by.
const std::string VectorscopeId = "org.sidescopes.vectorscope";
const std::string WaveformId = "org.sidescopes.waveform";
const std::string ParadeId = "org.sidescopes.parade";
const std::string HistogramId = "org.sidescopes.histogram";

// The whole captured frame, which most of these tests read. Stated rather than
// defaulted: a pass needs a region, and without one the worker computes
// nothing at all.
constexpr RegionOfInterest WholeFrame{0.0, 0.0, 100.0, 100.0};

bool waitFor(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

}  // namespace

TEST_CASE("Region edges round inward, never grabbing outside pixels")
{
    // At fractional positions truncation used to include the pixel row
    // just above the region - where the region border's bright ring is
    // drawn - and a phantom line flickered into the waveform.
    RegionOfInterest region;
    region.leftPercent = 10.03;
    region.topPercent = 10.03;
    region.rightPercent = 89.97;
    region.bottomPercent = 89.97;

    const IntRect rect = region.toPixels(1000, 1000);
    CHECK(rect.x == 101);
    CHECK(rect.y == 101);
    CHECK(rect.x + rect.width == 899);
    CHECK(rect.y + rect.height == 899);
}

TEST_CASE("AnalysisWorker produces scope images from published frames")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    // 75% red lands on the default BT.709 target (bin 109, 43).
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));
    CHECK(output.framesProcessed == 1);
}

TEST_CASE("AnalysisWorker applies settings that arrive before the first frame")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    // Let the worker consume the settings on a frameless pass - the case that
    // used to advance the settings version without configuring the scopes, so
    // the first frame after it published an output with no images.
    std::this_thread::sleep_for(50ms);
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE_FALSE(worker.fetchOutput(seen, output));  // no frame yet, nothing produced

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    // The pre-frame settings must still be in force: the first output carries
    // the vectorscope image they asked for, not an empty map.
    REQUIRE(output.images.count(VectorscopeId) == 1);
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));
}

TEST_CASE("AnalysisWorker survives rapid settings churn before a frame")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    // Several settings arrive back to back before any frame, each consumed on
    // its own frameless pass, so only the last enabled set may reach the first
    // output.
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {WaveformId};
    worker.updateSettings(settings);
    settings.enabledScopes = {HistogramId};
    worker.updateSettings(settings);
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);

    // Let the churn drain on frameless passes - the ordering that used to
    // advance the settings version without configuring the scopes, so the
    // first frame after it published an output with no images.
    std::this_thread::sleep_for(50ms);

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    // The last push wins: the first output holds the vectorscope it asked for
    // and none of the scopes the earlier pushes named.
    REQUIRE(output.images.count(VectorscopeId) == 1);
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));
    CHECK(output.images.count(WaveformId) == 0);
    CHECK(output.images.count(HistogramId) == 0);
}

TEST_CASE("A narrowed capture analyses the same content as a whole-display one")
{
    // The safety property of letting a frame describe its crop: the region is a
    // share of the DISPLAY, so narrowing the capture to it must not change which
    // pixels the scopes read. Two workers, same region, same display - one fed
    // the whole display, one fed only the region's own pixels - must agree
    // exactly.
    //
    // The display is 200x100: red on the left half, then green and blue splitting
    // the right half. The region is the right half, so it holds green AND blue.
    // A worker resolving the region against the narrowed FRAME rather than the
    // display would take only that frame's right half - blue alone - and miss the
    // green. The region's content has to be non-uniform for the test to see that;
    // a solid crop looks the same however much of it you read.
    RegionOfInterest rightHalf;
    rightHalf.leftPercent = 50.0;
    rightHalf.topPercent = 0.0;
    rightHalf.rightPercent = 100.0;
    rightHalf.bottomPercent = 100.0;

    const auto settingsFor = [&]() {
        AnalysisSettings settings;
        settings.enabledScopes = {VectorscopeId};
        settings.region = rightHalf;

        return settings;
    };

    const auto paintColumns = [](FrameBuffer& frame, int from, int to, Color color) {
        for (int py = 0; py < frame.height; ++py) {
            for (int px = from; px < to; ++px) {
                uint8_t* pixel = frame.data.data() + static_cast<std::size_t>(py) * frame.strideBytes +
                                 static_cast<std::size_t>(px) * 4;
                pixel[0] = color.b;
                pixel[1] = color.g;
                pixel[2] = color.r;
            }
        }
    };

    // The whole display: red | green | blue.
    FrameBuffer whole = makeSolidFrameBuffer(200, 100, Color{191, 0, 0}, 1);
    paintColumns(whole, 100, 150, Color{0, 191, 0});
    paintColumns(whole, 150, 200, Color{0, 0, 191});

    // The same display captured as only the region - green then blue - reporting
    // that it sits at x=100 of a 200x100 display.
    FrameBuffer narrowed = makeSolidFrameBuffer(100, 100, Color{0, 191, 0}, 1);
    paintColumns(narrowed, 50, 100, Color{0, 0, 191});
    narrowed.sourceX = 100;
    narrowed.sourceY = 0;
    narrowed.sourceWidth = 200;
    narrowed.sourceHeight = 100;

    const auto imageFrom = [&](FrameBuffer&& frame) {
        FrameMailbox mailbox;
        AnalysisWorker worker(mailbox);
        worker.updateSettings(settingsFor());
        worker.start();
        mailbox.publish(std::move(frame));
        uint64_t seen = 0;
        AnalysisWorker::Output output;
        REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

        return output.images.at(VectorscopeId);
    };

    const ScopeImage fromWhole = imageFrom(std::move(whole));
    const ScopeImage fromNarrowed = imageFrom(std::move(narrowed));

    REQUIRE(fromWhole.width == fromNarrowed.width);
    REQUIRE(fromWhole.height == fromNarrowed.height);
    REQUIRE_FALSE(fromWhole.rgba.empty());
    // Non-vacuous: the region really does carry a trace.
    const auto [brightX, brightY] = brightestPixel(fromWhole);
    CHECK(pixelLit(fromWhole, brightX, brightY));
    CHECK(fromWhole.rgba == fromNarrowed.rgba);
}

TEST_CASE("A narrowed capture never answers for a region it does not carry")
{
    // The crop trails the region by the reconfiguration's own latency. The
    // capture is asked to widen the instant the region moves, but the frame
    // already in hand is still the old crop - and a settings change is reason
    // enough to run a pass without a new frame. Measured against that frame the
    // moved region is silently clipped to whatever the old crop overlaps, and a
    // trace built from a fraction of the region is published as if it were the
    // whole of it. Nothing downstream can tell the difference.
    const auto settingsFor = [](RegionOfInterest region) {
        AnalysisSettings settings;
        settings.enabledScopes = {VectorscopeId};
        settings.region = region;

        return settings;
    };

    // A 200x100 display: red | green | blue. The crop is the right half, so it
    // holds green and blue and no red at all.
    const auto paintColumns = [](FrameBuffer& frame, int from, int to, Color color) {
        for (int py = 0; py < frame.height; ++py) {
            for (int px = from; px < to; ++px) {
                uint8_t* pixel = frame.data.data() + static_cast<std::size_t>(py) * frame.strideBytes +
                                 static_cast<std::size_t>(px) * 4;
                pixel[0] = color.b;
                pixel[1] = color.g;
                pixel[2] = color.r;
            }
        }
    };
    const auto wholeDisplay = [&](uint64_t sequence) {
        FrameBuffer frame = makeSolidFrameBuffer(200, 100, Color{191, 0, 0}, sequence);
        paintColumns(frame, 100, 150, Color{0, 191, 0});
        paintColumns(frame, 150, 200, Color{0, 0, 191});

        return frame;
    };
    const auto rightHalfCrop = [&](uint64_t sequence) {
        FrameBuffer frame = makeSolidFrameBuffer(100, 100, Color{0, 191, 0}, sequence);
        paintColumns(frame, 50, 100, Color{0, 0, 191});
        frame.sourceX = 100;
        frame.sourceY = 0;
        frame.sourceWidth = 200;
        frame.sourceHeight = 100;

        return frame;
    };

    // The region the crop was made for, and the one it is moved to: display
    // pixels 50..150, half red and half green, straddling the crop's left edge.
    const RegionOfInterest cropped{50.0, 0.0, 100.0, 100.0};
    const RegionOfInterest moved{25.0, 0.0, 75.0, 100.0};

    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.updateSettings(settingsFor(cropped));
    worker.start();
    mailbox.publish(rightHalfCrop(1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    const uint64_t beforeMove = seen;

    // The region moves off the crop. No frame carrying it has arrived yet, so
    // nothing may be published: the images standing on screen answer for the
    // old region, which is a truthful answer a moment out of date, where a pass
    // over the crop's overlap would be an untruthful one.
    worker.updateSettings(settingsFor(moved));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));
    for (int settle = 0; settle < 10; ++settle) {
        std::this_thread::sleep_for(5ms);
        REQUIRE_FALSE(worker.fetchOutput(seen, output));
    }
    CHECK(seen == beforeMove);

    // The widened frame is what answers, and it answers for the whole region.
    mailbox.publish(wholeDisplay(2));
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    FrameMailbox referenceMailbox;
    AnalysisWorker reference(referenceMailbox);
    reference.updateSettings(settingsFor(moved));
    reference.start();
    referenceMailbox.publish(wholeDisplay(1));
    uint64_t referenceSeen = 0;
    AnalysisWorker::Output referenceOutput;
    REQUIRE(waitFor([&] { return reference.fetchOutput(referenceSeen, referenceOutput); }));

    const ScopeImage& answered = output.images.at(VectorscopeId);
    const ScopeImage& expected = referenceOutput.images.at(VectorscopeId);
    // Non-vacuous: the region really does carry a trace, and it is not the one
    // the crop alone would have produced.
    const auto [brightX, brightY] = brightestPixel(expected);
    CHECK(pixelLit(expected, brightX, brightY));
    CHECK(answered.rgba == expected.rgba);
}

TEST_CASE("A scope hidden and shown again still produces an image")
{
    // A scope's instance is created when it appears and released when it goes
    // away, which is what stops a session paying for scopes nobody is looking at.
    // The risk that buys is the recreate path: if a returning scope were left
    // without an instance, or without its adaptive-image extension, it would go
    // quietly imageless.
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);

    const auto showing = [](const std::string& id) {
        AnalysisSettings settings;
        settings.region = WholeFrame;
        settings.enabledScopes = {id};
        // An image size only an adaptive scope honours, so a lost extension shows
        // up as the default size rather than this one.
        settings.imageSizes[id] = {1024, 384};

        return settings;
    };

    worker.updateSettings(showing(HistogramId));
    worker.start();
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 1));

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    REQUIRE(output.images.count(HistogramId) == 1);
    const int width = output.images.at(HistogramId).width;
    CHECK(width == 1024);

    // Away: something else entirely, so the histogram's instance is released.
    worker.updateSettings(showing(VectorscopeId));
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 2));
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output) && output.images.count(VectorscopeId) == 1; }));

    // Back again: a fresh instance, configured, drawing at the size it was asked
    // for rather than its module default.
    worker.updateSettings(showing(HistogramId));
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{140, 140, 140}, 3));
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output) && output.images.count(HistogramId) == 1; }));
    const ScopeImage& returned = output.images.at(HistogramId);
    CHECK(returned.width == width);
    CHECK_FALSE(returned.rgba.empty());
    const auto [brightX, brightY] = brightestPixel(returned);
    CHECK(pixelLit(returned, brightX, brightY));
}

TEST_CASE("AnalysisWorker skips frames with unchanged content")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    REQUIRE(output.framesProcessed == 1);
    const uint64_t versionAfterFirst = seen;

    // A second, identical frame is taken from the mailbox but its unchanged
    // content is not re-analyzed. Waiting for the worker to actually consume
    // it removes the race a fixed sleep only papered over: once the frame has
    // been taken, no new output can appear.
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 2));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 2; }));

    CHECK_FALSE(worker.fetchOutput(seen, output));
    CHECK(seen == versionAfterFirst);    // the output version never advanced
    CHECK(output.framesProcessed == 1);  // still the one analyzed frame
}

TEST_CASE("AnalysisWorker ignores changes inside the masked window")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.maskedWindow = IntRect{16, 16, 32, 32};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    const uint64_t versionAfterFirst = seen;
    FrameBuffer changedInsideMask = makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 2);
    changedInsideMask.data[static_cast<std::size_t>(24 * 64 + 24) * 4] = 250;  // inside the mask
    mailbox.publish(std::move(changedInsideMask));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 2; }));
    CHECK_FALSE(worker.fetchOutput(seen, output));
    CHECK(seen == versionAfterFirst);  // the masked change produced no output

    FrameBuffer changedOutsideMask = makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 3);
    changedOutsideMask.data[static_cast<std::size_t>(8 * 64 + 4) * 4] = 250;  // outside the mask
    mailbox.publish(std::move(changedOutsideMask));
    CHECK(waitFor([&] { return worker.fetchOutput(seen, output); }));
}

TEST_CASE("AnalysisWorker recomputes on settings changes without a new frame")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));  // BT.709 target
    const std::vector<uint8_t> boosted = output.images.at(VectorscopeId).rgba;

    // Switching the vectorscope's trace response redraws the same cloud on a
    // different density curve, and the worker recomputes on the frame it
    // already holds rather than waiting for a new one.
    settings.scopeParams[VectorscopeId]["response"] = 1.0;  // Linear
    worker.updateSettings(settings);
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    CHECK(output.images.at(VectorscopeId).rgba != boosted);
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));  // the same target
    CHECK(output.framesProcessed == 1);                         // same frame, reanalyzed
}

TEST_CASE("AnalysisWorker computes nothing without a region")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.enabledScopes = {VectorscopeId};  // shown, but reading nowhere
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));

    // The frame is taken - the readout still samples it - but no pass runs and
    // no output is published, so the scopes stay as empty as they started.
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(worker.fetchOutput(seen, output));
    CHECK(output.images.empty());
}

TEST_CASE("A region arriving late starts the scopes computing")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE_FALSE(worker.fetchOutput(seen, output));

    // Selecting a region reaches the worker as a settings change, which is
    // what re-creates the instances the empty state let go of - and it lands
    // on the frame already held, with no new one needed.
    settings.region = WholeFrame;
    worker.updateSettings(settings);
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    REQUIRE(output.images.count(VectorscopeId) == 1);
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));
}

TEST_CASE("AnalysisWorker keeps the scope set across a content-only pass")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    // Frame A configures the scopes and fills the vectorscope with 75% red.
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    REQUIRE(pixelLit(output.images.at(VectorscopeId), 109, 43));  // 75% red, BT.709

    // Frame B changes only the content, not the settings. The enabled set
    // persists when settingsChanged is false, so the vectorscope recomputes for
    // the new frame instead of dropping out of the output.
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{0, 191, 0}, 2));
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output) && output.framesProcessed == 2; }));
    REQUIRE(output.images.count(VectorscopeId) == 1);
    CHECK(brightestPixel(output.images.at(VectorscopeId)) != std::pair<int, int>{-1, -1});  // still lit
    CHECK_FALSE(pixelLit(output.images.at(VectorscopeId), 109, 43));                        // recomputed off 75% red
}

TEST_CASE("AnalysisWorker samples a cursor color from the latest frame")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{10, 150, 200}, 1));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));

    const auto sampled = worker.sampleDisplayColor(32, 32);
    REQUIRE(sampled.has_value());
    CHECK(sampled->r == 10.0f);
    CHECK(sampled->g == 150.0f);
    CHECK(sampled->b == 200.0f);
}

TEST_CASE("AnalysisWorker rejects out-of-bounds cursor samples")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{10, 150, 200}, 1));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));

    CHECK_FALSE(worker.sampleDisplayColor(-1, 5).has_value());
    CHECK_FALSE(worker.sampleDisplayColor(1000, 5).has_value());
}

TEST_CASE("A narrowed capture samples the display point, not the frame's")
{
    // The readout under the cursor asks in display pixels, so a capture
    // narrowed to part of its display has to map the question back into its own
    // frame. Reading the point as if it were a frame coordinate lands somewhere
    // else entirely - which is what froze the colour readout on a region.
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    // A 40x40 crop sitting at 100,60 of a 400x300 display: black, with one
    // white pixel at its own 10,5 - display 110,65.
    FrameBuffer narrowed = makeSolidFrameBuffer(40, 40, Color{0, 0, 0}, 1);
    uint8_t* white =
        narrowed.data.data() + static_cast<std::size_t>(5) * narrowed.strideBytes + static_cast<std::size_t>(10) * 4;
    white[0] = 255;
    white[1] = 255;
    white[2] = 255;
    narrowed.sourceX = 100;
    narrowed.sourceY = 60;
    narrowed.sourceWidth = 400;
    narrowed.sourceHeight = 300;
    mailbox.publish(std::move(narrowed));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));

    const auto onWhite = worker.sampleDisplayColor(110, 65, 0);
    REQUIRE(onWhite.has_value());
    CHECK(onWhite->r == 255.0f);

    // The same coordinates read as frame pixels would land on black, so the
    // mapping is what the assertion above rests on.
    const auto elsewhere = worker.sampleDisplayColor(120, 80, 0);
    REQUIRE(elsewhere.has_value());
    CHECK(elsewhere->r == 0.0f);

    // A point the crop does not carry has no answer at all; the caller falls
    // back to a one-shot screen read.
    CHECK_FALSE(worker.sampleDisplayColor(10, 10).has_value());
    CHECK_FALSE(worker.sampleDisplayColor(300, 200).has_value());
}

TEST_CASE("AnalysisWorker reports the latest frame size")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{10, 150, 200}, 1));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));

    const auto size = worker.latestFrameSize();
    REQUIRE(size.has_value());
    CHECK(size->width == 64);
    CHECK(size->height == 64);
}

TEST_CASE("A worker told to thin really thins the waveform's samples")
{
    // The path the application takes, not the engine's own: the setting has to
    // travel from AnalysisSettings, through configureScopes, into the module's
    // thinning extension and out the other side as a coarser pass. An earlier
    // extension shipped with every test driving the engine directly, and
    // deleting the module's wiring broke nothing at all.
    //
    // Black on exactly the rows a thinned pass visits and white everywhere
    // else, over a region a full pass reads whole: the trace's top lights for
    // the white rows only, so a pass that saw them differs visibly from one
    // that did not.
    constexpr int Columns = 256;
    constexpr int Width = 1400;
    constexpr int Height = 900;
    const IntRect region{0, 0, Width, Height};
    const long long full = budgetForBins(static_cast<long long>(Columns) * WaveformLevels, WaveformMinSamplesPerBin);
    REQUIRE(sampleGridFor(1, region, full).rowStride == 1);
    const SampleGrid thinned = sampleGridFor(1, region,
                                             budgetForBins(static_cast<long long>(Columns) * WaveformLevels,
                                                           WaveformMinSamplesPerBin / DraggedSampleDivisor));
    REQUIRE(thinned.rowStride > 1);

    const auto litTopOfTrace = [&](int divisor) {
        FrameMailbox mailbox;
        AnalysisWorker worker(mailbox);
        AnalysisSettings settings;
        settings.region = WholeFrame;
        settings.enabledScopes = {WaveformId};
        settings.imageSizes[WaveformId] = {Columns, WaveformLevels};
        settings.scopeParams[WaveformId]["mode"] = 1.0;  // luma
        settings.sampleThinning = divisor;
        worker.updateSettings(settings);
        worker.start();

        FrameBuffer frame = makeSolidFrameBuffer(Width, Height, Color{255, 255, 255}, 1);
        for (int index = 0; index < thinned.rows; ++index) {
            const int row = sampleRowOf(thinned, region, index);
            uint8_t* line = frame.data.data() + static_cast<std::size_t>(row) * Width * 4;
            std::fill_n(line, static_cast<std::size_t>(Width) * 4, uint8_t{0});
        }
        mailbox.publish(std::move(frame));

        uint64_t seen = 0;
        AnalysisWorker::Output output;
        REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
        const ScopeImage& image = output.images.at(WaveformId);

        return channelAt(image, image.width / 2, 0, 1);
    };

    CHECK(litTopOfTrace(1) > 0);
    CHECK(litTopOfTrace(DraggedSampleDivisor) == 0);
}

TEST_CASE("AnalysisWorker restricts analysis to the region of interest")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.enabledScopes = {VectorscopeId};
    settings.region = RegionOfInterest{0.0, 0.0, 50.0, 100.0};  // left half
    worker.updateSettings(settings);
    worker.start();

    // Left half 75% red, right half 75% blue: only red may appear.
    FrameBuffer frame = makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1);
    for (int py = 0; py < 64; ++py) {
        for (int px = 32; px < 64; ++px) {
            uint8_t* p = frame.data.data() + (static_cast<std::size_t>(py) * 64 + px) * 4;
            p[0] = 191;  // blue
            p[2] = 0;
        }
    }
    mailbox.publish(std::move(frame));

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));  // red target
    // Blue's BT.709 target: Cb = 112 * 191 / 256 + 128 = 211.6,
    // Cr = -10 * 191 / 256 + 128 = 120.5 -> pixel (212, 255 - 121).
    CHECK_FALSE(pixelLit(output.images.at(VectorscopeId), 212, 255 - 121));
}

TEST_CASE("AnalysisWorker routes every enabled scope and skips the disabled one")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    // Everything but the waveform: its output image must stay absent while
    // the parade, histogram, and outline - never asserted before - fill.
    settings.enabledScopes = {VectorscopeId, ParadeId, HistogramId};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    // The disabled waveform is never computed, so it has no output entry.
    CHECK(output.images.count(WaveformId) == 0);

    // Every enabled scope produced a lit image.
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));  // 75% red, BT.709
    CHECK(brightestPixel(output.images.at(ParadeId)) != std::pair<int, int>{-1, -1});
    CHECK(brightestPixel(output.images.at(HistogramId)) != std::pair<int, int>{-1, -1});

    // The histogram outline rides alongside its image: three channels of
    // 256 bin heights, at least one of them raised.
    REQUIRE(output.histogramOutline.size() == static_cast<std::size_t>(3) * 256);
    CHECK(*std::max_element(output.histogramOutline.begin(), output.histogramOutline.end()) > 0.0f);
}

TEST_CASE("AnalysisWorker sampleDisplayColor honors the averaging radius")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    // A single red corner pixel on an otherwise black frame: radius 0 reads
    // that pixel alone, radius 1 averages it with its three black neighbors.
    FrameBuffer frame = makeSolidFrameBuffer(4, 4, Color{0, 0, 0}, 1);
    frame.data[2] = 255;  // red at pixel (0, 0)
    mailbox.publish(std::move(frame));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));

    const auto tight = worker.sampleDisplayColor(0, 0, 0);
    const auto wide = worker.sampleDisplayColor(0, 0, 1);
    REQUIRE(tight.has_value());
    REQUIRE(wide.has_value());
    CHECK(tight->r == 255.0f);
    CHECK(wide->r == 63.75f);  // one red among four clipped samples: 255 / 4
    CHECK(wide->r < tight->r);
}

TEST_CASE("A held worker publishes nothing, however many frames arrive")
{
    // What an attached window being dragged across the screen asks for: the
    // region is in transit, so there is nothing worth reading off it, and the
    // last images stand until it lands.
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    const uint64_t heldAt = seen;

    worker.hold(true);
    CHECK(worker.held());
    // Each frame carries different content, so a worker that failed to hold
    // would have every reason to publish rather than skipping on the
    // unchanged-content test.
    for (uint64_t sequence = 2; sequence <= 5; ++sequence) {
        const auto red = static_cast<uint8_t>(40 * sequence);
        mailbox.publish(makeSolidFrameBuffer(64, 64, Color{red, 0, 0}, sequence));
        REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == sequence; }));
    }
    // Every frame is still taken - the colour under the pointer is read from
    // it, and that goes on working while the region is in transit - but none
    // of them is analysed.
    CHECK_FALSE(worker.fetchOutput(seen, output));
    CHECK(seen == heldAt);
}

TEST_CASE("Releasing a held worker recomputes without waiting for a frame")
{
    // The region moves all the way through the hold, so the settings changes
    // that carried it were consumed while nothing ran. Nothing else records
    // that the images on screen answer for somewhere the region has left.
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    worker.hold(true);
    settings.region = RegionOfInterest{0.0, 0.0, 50.0, 50.0};
    worker.updateSettings(settings);
    // Two held passes, each of which takes a frame and then reads the pending
    // settings, so by the second the region change has certainly been consumed
    // with nothing computed from it.
    for (uint64_t sequence = 2; sequence <= 3; ++sequence) {
        mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, sequence));
        REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == sequence; }));
    }
    CHECK_FALSE(worker.fetchOutput(seen, output));

    // No frame is published after this: the release alone has to be reason
    // enough to run.
    worker.hold(false);
    CHECK_FALSE(worker.held());
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));
}

TEST_CASE("A released frame is let go and the accessors say so")
{
    // A whole display's pixels are the largest thing this application holds,
    // and a suspended capture will never replace them. Letting the frame go is
    // what turns the pause into a memory saving; the accessors then answering
    // nothing is what sends the colour readout to its off-stream sample rather
    // than reporting a picture of the screen as it used to be.
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = WholeFrame;
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    REQUIRE(waitFor([&] { return worker.consumedFrameSequence() == 1; }));
    REQUIRE(worker.sampleDisplayColor(32, 32).has_value());
    REQUIRE(worker.latestFrameSize().has_value());

    worker.releaseFrame();
    REQUIRE(waitFor([&] { return !worker.latestFrameSize().has_value(); }));
    CHECK_FALSE(worker.sampleDisplayColor(32, 32).has_value());
    CHECK_FALSE(worker.withLatestFrame([](const FrameView&) {}));

    // And the pipeline picks up again from the next frame, at full health.
    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{0, 191, 0}, 2));
    REQUIRE(waitFor([&] { return worker.latestFrameSize().has_value(); }));
    const std::optional<FloatColor> resumed = worker.sampleDisplayColor(32, 32);
    REQUIRE(resumed.has_value());
    CHECK(resumed->g > resumed->r);
}

TEST_CASE("AnalysisWorker offers no frame color before one arrives")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    // No frame has been published, so the frame accessors stay empty rather
    // than reading uninitialized storage.
    CHECK_FALSE(worker.sampleDisplayColor(0, 0).has_value());
    CHECK_FALSE(worker.latestFrameSize().has_value());
}

}  // namespace sidescopes
