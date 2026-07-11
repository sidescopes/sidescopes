#include "core/scopes/histogram.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

Histogram::Histogram() : bins_(static_cast<std::size_t>(kBins) * 3, 0) {
    image_.width = kImageWidth;
    image_.height = kHeight;
    image_.rgba.assign(static_cast<std::size_t>(kImageWidth) * kHeight * 4, 0);
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
    uint32_t* blue_bins = bins_.data() + static_cast<std::ptrdiff_t>(2) * kBins;

    if (!region.Empty()) {
        const int stride = settings_.sampling_stride;
        for (int py = region.y; py < region.y + region.height; py += stride) {
            const uint8_t* row = frame.PixelAt(region.x, py);
            for (int px = 0; px < region.width; px += stride) {
                const uint8_t* pixel = row + static_cast<std::size_t>(px) * 4;
                ++blue_bins[pixel[0]];
                ++green_bins[pixel[1]];
                ++red_bins[pixel[2]];
            }
        }
    }
    MapBinsToImage();
}

void Histogram::MapBinsToImage() {
    // A gaussian-ish 1-4-6-4-1 kernel against bin-to-bin comb: real
    // photographs produce ragged neighboring counts that read as noise at
    // this size.
    std::vector<double> smoothed(static_cast<std::size_t>(kBins) * 3, 0.0);
    for (int channel = 0; channel < 3; ++channel) {
        const uint32_t* plane = bins_.data() + static_cast<std::ptrdiff_t>(channel) * kBins;
        double* out = smoothed.data() + static_cast<std::ptrdiff_t>(channel) * kBins;
        for (int value = 0; value < kBins; ++value) {
            const auto at = [&](int index) { return plane[std::clamp(index, 0, kBins - 1)]; };
            out[value] = (at(value - 2) + 4.0 * at(value - 1) + 6.0 * at(value) +
                          4.0 * at(value + 1) + at(value + 2)) /
                         16.0;
        }
    }

    double densest = 0.0;
    for (const double count : smoothed) densest = std::max(densest, count);

    // Square-root heights, normalized to the tallest bin: the photo
    // editors' compromise. Linear demands per-image zoom the moment one
    // tone dominates, and a log curve melts every spike into a plateau;
    // the square root keeps a population spike a spike while sparse
    // tonal tails stay visible. Parameter-free by design.
    const auto bar_height = [&](double count) -> double {
        if (count <= 0.0 || densest <= 0.0) return 0.0;
        return std::max(1.0, std::sqrt(count / densest) * kHeight);
    };

    // Bin heights once, then the filled area under a Catmull-Rom spline
    // through them, evaluated per image column with a fractional-coverage
    // top pixel: a plotted function with no kinks at the bins.
    std::vector<double> heights(static_cast<std::size_t>(kBins) * 3);
    for (int channel = 0; channel < 3; ++channel)
        for (int value = 0; value < kBins; ++value)
            heights[static_cast<std::size_t>(channel) * kBins + value] =
                bar_height(smoothed[static_cast<std::size_t>(channel) * kBins + value]);

    // Combined: three channels overlaid over the full height. Per
    // channel: three stacked bands, each holding one channel's plot.
    const bool split = settings_.style == HistogramStyle::PerChannel;
    const int band_height = split ? kHeight / 3 : kHeight;
    std::fill(image_.rgba.begin(), image_.rgba.end(), uint8_t{0});
    for (int x = 0; x < kImageWidth; ++x) {
        const double bin_position =
            std::clamp((x + 0.5) * kBins / kImageWidth - 0.5, 0.0, kBins - 1.0);
        const int center = static_cast<int>(bin_position);
        const double t = bin_position - center;
        for (int channel = 0; channel < 3; ++channel) {
            const double* plane = heights.data() + static_cast<std::ptrdiff_t>(channel) * kBins;
            const auto at = [&](int index) { return plane[std::clamp(index, 0, kBins - 1)]; };
            const double p0 = at(center - 1);
            const double p1 = at(center);
            const double p2 = at(center + 1);
            const double p3 = at(center + 2);
            double height =
                p1 +
                0.5 * t *
                    (p2 - p0 +
                     t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
            // The spline may overshoot near sharp features; the plot
            // stays within the panel, and stretches between empty bins
            // stay empty.
            if (p1 <= 0.0 && p2 <= 0.0) height = 0.0;
            height = std::clamp(height, 0.0, static_cast<double>(kHeight));
            if (split) height /= 3.0;
            if (height <= 0.0) continue;
            // Red on top, then green, then blue, when split.
            const int band_bottom = split ? (channel + 1) * band_height : kHeight;
            // The top edge fades over a few rows rather than one: the
            // pane can magnify the plot, and a single-pixel edge would
            // come back as a ladder on the slopes.
            constexpr double kFeather = 2.5;
            const double top = band_bottom - height;
            const int first_touched =
                std::max(band_bottom - band_height, static_cast<int>(std::floor(top - kFeather)));
            for (int row = first_touched; row < band_bottom; ++row) {
                const double coverage = std::clamp((row + 1.0 - top) / kFeather, 0.0, 1.0);
                if (coverage <= 0.0) continue;
                image_.rgba[(static_cast<std::size_t>(row) * kImageWidth + x) * 4 + channel] =
                    static_cast<uint8_t>(210 * coverage);
            }
        }
        for (int row = 0; row < kHeight; ++row)
            image_.rgba[(static_cast<std::size_t>(row) * kImageWidth + x) * 4 + 3] = 255;
    }
    ++image_.sequence;
}

}  // namespace sidescopes
