#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "core/analysis_worker.h"

namespace sidescopes {
namespace {

using namespace std::chrono_literals;

FrameBuffer makeSolidFrame(int width, int height, Color color, uint64_t sequence)
{
    FrameBuffer frame;
    frame.width = width;
    frame.height = height;
    frame.strideBytes = width * 4;
    frame.colorSpace = ColorSpaceHint::Srgb;
    frame.sequence = sequence;
    frame.data.resize(static_cast<std::size_t>(frame.strideBytes) * height);
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            uint8_t* p =
                frame.data.data() + static_cast<std::size_t>(py) * frame.strideBytes + static_cast<std::size_t>(px) * 4;
            p[0] = color.b;
            p[1] = color.g;
            p[2] = color.r;
            p[3] = 255;
        }
    }
    return frame;
}

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

bool pixelLit(const ScopeImage& image, int px, int py)
{
    const uint8_t* rgba = image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
    return rgba[0] + rgba[1] + rgba[2] > 0;
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

    mailbox.publish(makeSolidFrame(64, 64, Color{191, 0, 0}, 1));

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

    mailbox.publish(makeSolidFrame(64, 64, Color{100, 100, 100}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    mailbox.publish(makeSolidFrame(64, 64, Color{100, 100, 100}, 2));
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

    mailbox.publish(makeSolidFrame(64, 64, Color{100, 100, 100}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitFor([&] { return worker.fetchOutput(seen, output); }));

    FrameBuffer changedInsideMask = makeSolidFrame(64, 64, Color{100, 100, 100}, 2);
    changedInsideMask.data[static_cast<std::size_t>(24 * 64 + 24) * 4] = 250;  // inside the mask
    mailbox.publish(std::move(changedInsideMask));
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(worker.fetchOutput(seen, output));

    FrameBuffer changedOutsideMask = makeSolidFrame(64, 64, Color{100, 100, 100}, 3);
    changedOutsideMask.data[static_cast<std::size_t>(8 * 64 + 4) * 4] = 250;  // outside the mask
    mailbox.publish(std::move(changedOutsideMask));
    CHECK(waitFor([&] { return worker.fetchOutput(seen, output); }));
}

TEST_CASE("AnalysisWorker recomputes on settings changes without a new frame")
{
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.start();

    mailbox.publish(makeSolidFrame(64, 64, Color{191, 0, 0}, 1));
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

    mailbox.publish(makeSolidFrame(64, 64, Color{10, 150, 200}, 1));
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
    FrameBuffer frame = makeSolidFrame(64, 64, Color{191, 0, 0}, 1);
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
