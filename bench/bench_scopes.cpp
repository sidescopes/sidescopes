#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "core/frame.h"
#include "core/scopes/histogram.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"

namespace sidescopes {
namespace {

struct SyntheticFrame
{
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;

    [[nodiscard]] FrameView view() const
    {
        return FrameView{pixels.data(), width * 4, width, height, ColorSpaceHint::Srgb, 1};
    }

    [[nodiscard]] IntRect fullRegion() const
    {
        return IntRect{0, 0, width, height};
    }
};

constexpr uint8_t ToByte(int value)
{
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// A gradient-plus-noise BGRA frame: the ramp spreads chroma and luma across
// the whole scope, and the fixed-seed jitter populates the fine bin structure
// the density-correction paths work on. Built from a fixed seed so runs on the
// same machine are comparable; no clock or environment enters the pixels.
SyntheticFrame MakeGradientNoiseFrame(int width, int height)
{
    SyntheticFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<std::size_t>(width) * height * 4);

    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> jitter(-12, 12);
    const int spanBase = width + height - 2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int rampR = x * 255 / (width - 1);
            const int rampG = y * 255 / (height - 1);
            const int rampB = (x + y) * 255 / spanBase;
            uint8_t* pixel = frame.pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            pixel[0] = ToByte(rampB + jitter(rng));
            pixel[1] = ToByte(rampG + jitter(rng));
            pixel[2] = ToByte(rampR + jitter(rng));
            pixel[3] = 255;
        }
    }

    return frame;
}

}  // namespace

TEST_CASE("scope engines accumulate synthetic frames", "[bench]")
{
    const SyntheticFrame frame1080 = MakeGradientNoiseFrame(1920, 1080);
    const SyntheticFrame frame2160 = MakeGradientNoiseFrame(3840, 2160);

    Vectorscope vectorscope;
    Waveform waveform;
    Histogram histogram;

    BENCHMARK("vectorscope accumulate 1080p")
    {
        vectorscope.accumulate(frame1080.view(), frame1080.fullRegion());

        return vectorscope.image().rgba[0];
    };

    BENCHMARK("vectorscope accumulate 4k")
    {
        vectorscope.accumulate(frame2160.view(), frame2160.fullRegion());

        return vectorscope.image().rgba[0];
    };

    BENCHMARK("waveform accumulate 1080p")
    {
        waveform.accumulate(frame1080.view(), frame1080.fullRegion());

        return waveform.image().rgba[0];
    };

    BENCHMARK("waveform accumulate 4k")
    {
        waveform.accumulate(frame2160.view(), frame2160.fullRegion());

        return waveform.image().rgba[0];
    };

    BENCHMARK("histogram accumulate 1080p")
    {
        histogram.accumulate(frame1080.view(), frame1080.fullRegion());

        return histogram.image().rgba[0];
    };

    BENCHMARK("histogram accumulate 4k")
    {
        histogram.accumulate(frame2160.view(), frame2160.fullRegion());

        return histogram.image().rgba[0];
    };
}

}  // namespace sidescopes
