#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include "core/analysis_worker.h"

namespace sidescopes {
namespace {

using namespace std::chrono_literals;

FrameBuffer MakeSolidFrame(int width, int height, Color color, uint64_t sequence) {
    FrameBuffer frame;
    frame.width = width;
    frame.height = height;
    frame.stride_bytes = width * 4;
    frame.color_space = ColorSpaceHint::Srgb;
    frame.sequence = sequence;
    frame.data.resize(static_cast<std::size_t>(frame.stride_bytes) * height);
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            uint8_t* p = frame.data.data() + static_cast<std::size_t>(py) * frame.stride_bytes +
                         static_cast<std::size_t>(px) * 4;
            p[0] = color.b;
            p[1] = color.g;
            p[2] = color.r;
            p[3] = 255;
        }
    }
    return frame;
}

bool WaitFor(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

bool PixelLit(const ScopeImage& image, int px, int py) {
    const uint8_t* rgba = image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
    return rgba[0] + rgba[1] + rgba[2] > 0;
}

}  // namespace

TEST_CASE("Region edges round inward, never grabbing outside pixels") {
    // At fractional positions truncation used to include the pixel row
    // just above the region - where the region border's bright ring is
    // drawn - and a phantom line flickered into the waveform.
    RegionOfInterest region;
    region.left_percent = 10.03;
    region.top_percent = 10.03;
    region.right_percent = 89.97;
    region.bottom_percent = 89.97;

    const IntRect rect = region.ToPixels(1000, 1000);
    CHECK(rect.x == 101);
    CHECK(rect.y == 101);
    CHECK(rect.x + rect.width == 899);
    CHECK(rect.y + rect.height == 899);
}

TEST_CASE("AnalysisWorker produces scope images from published frames") {
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.Start();

    mailbox.Publish(MakeSolidFrame(64, 64, Color{191, 0, 0}, 1));

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(WaitFor([&] { return worker.FetchOutput(seen, output); }));
    // 75% red lands on the default BT.709 target (bin 109, 43).
    CHECK(PixelLit(output.vectorscope_image, 109, 43));
    CHECK(output.frames_processed == 1);
}

TEST_CASE("AnalysisWorker reports the region's average color") {
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.Start();

    mailbox.Publish(MakeSolidFrame(64, 64, Color{100, 150, 200}, 1));

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(WaitFor([&] { return worker.FetchOutput(seen, output); }));
    REQUIRE(output.region_average_valid);
    CHECK(output.region_average.r == 100.0f);
    CHECK(output.region_average.g == 150.0f);
    CHECK(output.region_average.b == 200.0f);
}

TEST_CASE("AnalysisWorker skips frames with unchanged content") {
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.Start();

    mailbox.Publish(MakeSolidFrame(64, 64, Color{100, 100, 100}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(WaitFor([&] { return worker.FetchOutput(seen, output); }));

    mailbox.Publish(MakeSolidFrame(64, 64, Color{100, 100, 100}, 2));
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(worker.FetchOutput(seen, output));
}

TEST_CASE("AnalysisWorker ignores changes inside the masked window") {
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.masked_window = IntRect{16, 16, 32, 32};
    worker.UpdateSettings(settings);
    worker.Start();

    mailbox.Publish(MakeSolidFrame(64, 64, Color{100, 100, 100}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(WaitFor([&] { return worker.FetchOutput(seen, output); }));

    FrameBuffer changed_inside_mask = MakeSolidFrame(64, 64, Color{100, 100, 100}, 2);
    changed_inside_mask.data[static_cast<std::size_t>(24 * 64 + 24) * 4] = 250;  // inside the mask
    mailbox.Publish(std::move(changed_inside_mask));
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(worker.FetchOutput(seen, output));

    FrameBuffer changed_outside_mask = MakeSolidFrame(64, 64, Color{100, 100, 100}, 3);
    changed_outside_mask.data[static_cast<std::size_t>(8 * 64 + 4) * 4] = 250;  // outside the mask
    mailbox.Publish(std::move(changed_outside_mask));
    CHECK(WaitFor([&] { return worker.FetchOutput(seen, output); }));
}

TEST_CASE("AnalysisWorker recomputes on settings changes without a new frame") {
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.Start();

    mailbox.Publish(MakeSolidFrame(64, 64, Color{191, 0, 0}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(WaitFor([&] { return worker.FetchOutput(seen, output); }));
    CHECK(PixelLit(output.vectorscope_image, 109, 43));  // default BT.709 target

    AnalysisSettings settings;
    settings.vectorscope.matrix = ChromaMatrix::Bt601;
    worker.UpdateSettings(settings);
    REQUIRE(WaitFor([&] { return worker.FetchOutput(seen, output); }));
    CHECK(PixelLit(output.vectorscope_image, 100, 43));  // BT.601 target
    CHECK(output.frames_processed == 1);                 // same frame, reanalyzed
}

TEST_CASE("AnalysisWorker samples cursor colors from the latest frame") {
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    worker.Start();

    mailbox.Publish(MakeSolidFrame(64, 64, Color{10, 150, 200}, 1));
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(WaitFor([&] { return worker.FetchOutput(seen, output); }));

    const auto sampled = worker.SampleFrameColor(32, 32);
    REQUIRE(sampled.has_value());
    CHECK(sampled->r == 10.0f);
    CHECK(sampled->g == 150.0f);
    CHECK(sampled->b == 200.0f);

    CHECK_FALSE(worker.SampleFrameColor(-1, 5).has_value());
    CHECK_FALSE(worker.SampleFrameColor(1000, 5).has_value());

    const auto size = worker.LatestFrameSize();
    REQUIRE(size.has_value());
    CHECK(size->width == 64);
    CHECK(size->height == 64);
}

TEST_CASE("AnalysisWorker restricts analysis to the region of interest") {
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    AnalysisSettings settings;
    settings.region = RegionOfInterest{0.0, 0.0, 50.0, 100.0};  // left half
    worker.UpdateSettings(settings);
    worker.Start();

    // Left half 75% red, right half 75% blue: only red may appear.
    FrameBuffer frame = MakeSolidFrame(64, 64, Color{191, 0, 0}, 1);
    for (int py = 0; py < 64; ++py) {
        for (int px = 32; px < 64; ++px) {
            uint8_t* p = frame.data.data() + (static_cast<std::size_t>(py) * 64 + px) * 4;
            p[0] = 191;  // blue
            p[2] = 0;
        }
    }
    mailbox.Publish(std::move(frame));

    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(WaitFor([&] { return worker.FetchOutput(seen, output); }));
    CHECK(PixelLit(output.vectorscope_image, 109, 43));  // red target
    // Blue's BT.709 target: Cb = 112 * 191 / 256 + 128 = 211.6,
    // Cr = -10 * 191 / 256 + 128 = 120.5 -> pixel (212, 255 - 121).
    CHECK_FALSE(PixelLit(output.vectorscope_image, 212, 255 - 121));
}

}  // namespace sidescopes
