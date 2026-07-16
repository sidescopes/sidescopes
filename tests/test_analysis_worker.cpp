#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "core/analysis_worker.h"
#include "scope_image.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

namespace {

using namespace std::chrono_literals;

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
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    // 75% red lands on the default BT.709 target (bin 109, 43).
    CHECK(pixelLit(output.vectorscopeImage, 109, 43));
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

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 2));
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(worker.fetchOutput(seen, output));
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

    FrameBuffer changedInsideMask = makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 2);
    changedInsideMask.data[static_cast<std::size_t>(24 * 64 + 24) * 4] = 250;  // inside the mask
    mailbox.publish(std::move(changedInsideMask));
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(worker.fetchOutput(seen, output));

    FrameBuffer changedOutsideMask = makeSolidFrameBuffer(64, 64, Color{100, 100, 100}, 3);
    changedOutsideMask.data[static_cast<std::size_t>(8 * 64 + 4) * 4] = 250;  // outside the mask
    mailbox.publish(std::move(changedOutsideMask));
    CHECK(waitFor([&] { return worker.fetchOutput(seen, output); }));
}

TEST_CASE("AnalysisWorker recomputes on settings changes without a new frame")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    CHECK(pixelLit(output.vectorscopeImage, 109, 43));  // default BT.709 target

    AnalysisSettings settings;
    settings.vectorscope.matrix = ChromaMatrix::Bt601;
    worker.updateSettings(settings);
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));
    CHECK(pixelLit(output.vectorscopeImage, 100, 43));  // BT.601 target
    CHECK(output.framesProcessed == 1);                 // same frame, reanalyzed
}

TEST_CASE("AnalysisWorker samples cursor colors from the latest frame")
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

    CHECK_FALSE(worker.sampleFrameColor(-1, 5).has_value());
    CHECK_FALSE(worker.sampleFrameColor(1000, 5).has_value());

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
    CHECK(pixelLit(output.vectorscopeImage, 109, 43));  // red target
    // Blue's BT.709 target: Cb = 112 * 191 / 256 + 128 = 211.6,
    // Cr = -10 * 191 / 256 + 128 = 120.5 -> pixel (212, 255 - 121).
    CHECK_FALSE(pixelLit(output.vectorscopeImage, 212, 255 - 121));
}

}  // namespace sidescopes
