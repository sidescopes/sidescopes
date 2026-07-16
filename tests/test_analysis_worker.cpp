#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "core/analysis_worker.h"
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

TEST_CASE("AnalysisWorker skips frames with unchanged content")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
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
    settings.enabledScopes = {VectorscopeId};
    worker.updateSettings(settings);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    CHECK(pixelLit(output.images.at(VectorscopeId), 109, 43));  // default BT.709 target

    // Switching the vectorscope's chroma matrix to BT.601 moves 75% red's
    // target, and the worker recomputes on the frame it already holds.
    settings.scopeParams[VectorscopeId]["matrix"] = 0.0;
    worker.updateSettings(settings);
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    CHECK(pixelLit(output.images.at(VectorscopeId), 100, 43));  // BT.601 target
    CHECK(output.framesProcessed == 1);                         // same frame, reanalyzed
}

TEST_CASE("AnalysisWorker samples a cursor color from the latest frame")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{10, 150, 200}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    const auto sampled = worker.sampleFrameColor(32, 32);
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
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    CHECK_FALSE(worker.sampleFrameColor(-1, 5).has_value());
    CHECK_FALSE(worker.sampleFrameColor(1000, 5).has_value());
}

TEST_CASE("AnalysisWorker reports the latest frame size")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{10, 150, 200}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    const auto size = worker.latestFrameSize();
    REQUIRE(size.has_value());
    CHECK(size->width == 64);
    CHECK(size->height == 64);
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

TEST_CASE("AnalysisWorker sampleFrameColor honors the averaging radius")
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

    const auto tight = worker.sampleFrameColor(0, 0, 0);
    const auto wide = worker.sampleFrameColor(0, 0, 1);
    REQUIRE(tight.has_value());
    REQUIRE(wide.has_value());
    CHECK(tight->r == 255.0f);
    CHECK(wide->r == 63.75f);  // one red among four clipped samples: 255 / 4
    CHECK(wide->r < tight->r);
}

TEST_CASE("AnalysisWorker offers no frame color before one arrives")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    // No frame has been published, so the frame accessors stay empty rather
    // than reading uninitialized storage.
    CHECK_FALSE(worker.sampleFrameColor(0, 0).has_value());
    CHECK_FALSE(worker.latestFrameSize().has_value());
}

}  // namespace sidescopes
