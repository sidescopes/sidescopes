#include "core/scopes/histogram.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

constexpr double kReferenceSampleCount = 1'000'000.0;

}  // namespace

Histogram::Histogram() : bins_(static_cast<std::size_t>(kBins) * 3, 0) {
    image_.width = kBins;
    image_.height = kHeight;
    image_.rgba.assign(static_cast<std::size_t>(kBins) * kHeight * 4, 0);
}

void Histogram::Configure(const HistogramSettings& settings) {
    settings_ = settings;
    settings_.sampling_stride = std::clamp(settings_.sampling_stride, 1, 8);
}

void Histogram::Accumulate(const FrameView& frame, IntRect region) {
    region = region.ClampedTo(frame.width, frame.height);
    std::fill(bins_.begin(), bins_.end(), 0u);

    uint32_t* red_bins = bins_.data();
    uint32_t* green_bins = bins_.data() + kBins;
    uint32_t* blue_bins = bins_.data() + 2 * kBins;

    uint64_t sample_count = 0;
    if (!region.Empty()) {
        const int stride = settings_.sampling_stride;
        for (int py = region.y; py < region.y + region.height; py += stride) {
            const uint8_t* row = frame.PixelAt(region.x, py);
            for (int px = 0; px < region.width; px += stride) {
                const uint8_t* pixel = row + static_cast<std::size_t>(px) * 4;
                ++blue_bins[pixel[0]];
                ++green_bins[pixel[1]];
                ++red_bins[pixel[2]];
                ++sample_count;
            }
        }
    }
    MapBinsToImage(sample_count);
}

void Histogram::MapBinsToImage(uint64_t sample_count) {
    uint32_t densest = 0;
    for (const uint32_t count : bins_) densest = std::max(densest, count);

    const double per_sample_scale =
        sample_count > 0 ? kReferenceSampleCount / static_cast<double>(sample_count) : 0.0;
    const double gain = static_cast<double>(settings_.gain) * per_sample_scale;
    const double log_ceiling = densest > 0 ? std::log1p(static_cast<double>(densest) * gain) : 0.0;
    const auto bar_height = [&](uint32_t count) -> int {
        if (count == 0 || log_ceiling <= 0.0) return 0;
        const double normalized = std::log1p(static_cast<double>(count) * gain) / log_ceiling;
        return std::max(1, static_cast<int>(normalized * kHeight));
    };

    const uint32_t* planes[3] = {bins_.data(), bins_.data() + kBins, bins_.data() + 2 * kBins};
    std::fill(image_.rgba.begin(), image_.rgba.end(), uint8_t{0});
    for (int value = 0; value < kBins; ++value) {
        for (int channel = 0; channel < 3; ++channel) {
            const int height = bar_height(planes[channel][value]);
            for (int row = kHeight - height; row < kHeight; ++row) {
                image_.rgba[(static_cast<std::size_t>(row) * kBins + value) * 4 + channel] = 210;
            }
        }
        for (int row = 0; row < kHeight; ++row)
            image_.rgba[(static_cast<std::size_t>(row) * kBins + value) * 4 + 3] = 255;
    }
    ++image_.sequence;
}

}  // namespace sidescopes
