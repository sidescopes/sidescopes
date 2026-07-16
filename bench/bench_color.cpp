#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "core/color_lab.h"
#include "core/frame.h"

namespace sidescopes {
namespace {

// A deterministic batch of sRGB colours spread across the cube, enough to keep
// the transfer function and XYZ->Lab path honest without any external data.
std::vector<FloatColor> MakeSrgbBatch(std::size_t count, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> channel(0.0f, 255.0f);
    std::vector<FloatColor> colors(count);
    for (FloatColor& color : colors) {
        color = FloatColor{channel(rng), channel(rng), channel(rng)};
    }

    return colors;
}

std::vector<LabColor> MakeLabBatch(std::size_t count, uint32_t seed)
{
    const std::vector<FloatColor> srgb = MakeSrgbBatch(count, seed);
    std::vector<LabColor> lab(count);
    for (std::size_t i = 0; i < count; ++i) {
        lab[i] = labFromSrgb(srgb[i]);
    }

    return lab;
}

}  // namespace

TEST_CASE("color_lab conversions and differences", "[bench]")
{
    const std::vector<FloatColor> srgbBatch = MakeSrgbBatch(256, 0x1A2B3Cu);
    const std::vector<LabColor> firstBatch = MakeLabBatch(1000, 0x4D5E6Fu);
    const std::vector<LabColor> secondBatch = MakeLabBatch(1000, 0x708192u);

    BENCHMARK("color_lab labFromSrgb 256")
    {
        float sum = 0.0f;
        for (const FloatColor& color : srgbBatch) {
            sum += labFromSrgb(color).lightness;
        }

        return sum;
    };

    BENCHMARK("color_lab deltaE2000 1k")
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < firstBatch.size(); ++i) {
            sum += deltaE2000(firstBatch[i], secondBatch[i]);
        }

        return sum;
    };

    BENCHMARK("color_lab differenceFrom 1k")
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < firstBatch.size(); ++i) {
            sum += differenceFrom(firstBatch[i], secondBatch[i]).deltaE;
        }

        return sum;
    };
}

}  // namespace sidescopes
