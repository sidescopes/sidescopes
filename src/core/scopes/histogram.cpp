#include "core/scopes/histogram.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

Histogram::Histogram() : bins_(static_cast<std::size_t>(kBins) * 3, 0) {
    Resize(kImageWidth, kHeight);
}

void Histogram::Resize(int width, int height) {
    width_ = width;
    height_ = height;
    image_.width = width_;
    image_.height = height_;
    image_.rgba.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
}

void Histogram::Configure(const HistogramSettings& settings) {
    settings_ = settings;
    settings_.sampling_stride = std::clamp(settings_.sampling_stride, 1, 8);
    settings_.image_width = std::clamp(settings_.image_width, kBins, 4096);
    settings_.image_height = std::clamp(settings_.image_height, 192, 1536);
    if (settings_.image_width != width_ || settings_.image_height != height_)
        Resize(settings_.image_width, settings_.image_height);
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
        return std::max(1.0, std::sqrt(count / densest) * height_);
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
    // Each plot is a dim solid fill under a bright solid outline riding
    // the curve - the fill shows the area at a glance, the outline
    // carries the shape and stays traceable through every overlap.
    // Overlaps sum per component, so two dim fills make a dim secondary
    // and all three make a quiet neutral gray. Gradients were tried and
    // retired: they read as texture and drifted on varied photos.
    const bool split = settings_.style == HistogramStyle::PerChannel;
    const int band_height = split ? height_ / 3 : height_;
    constexpr double kFillValue = 118.0;
    constexpr double kOutlineValue = 235.0;
    // Curve tops for every column first: the outline stroke must span
    // the gap between neighboring tops, or steep slopes and sharp peaks
    // break it into dashes.
    static thread_local std::vector<double> tops;
    tops.assign(static_cast<std::size_t>(width_) * 3, static_cast<double>(height_));
    for (int x = 0; x < width_; ++x) {
        const double bin_position = std::clamp((x + 0.5) * kBins / width_ - 0.5, 0.0, kBins - 1.0);
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
            height = std::clamp(height, 0.0, static_cast<double>(height_));
            if (split) height /= 3.0;
            if (height <= 0.0) continue;
            const int band_bottom = split ? (channel + 1) * band_height : height_;
            tops[static_cast<std::size_t>(channel) * width_ + x] = band_bottom - height;
        }
    }
    // Squared distance from a point to a segment, for the outline's
    // anti-aliasing: the stroke is a true line, equally thick on flats,
    // slopes, and vertical spikes.
    const auto segment_distance = [](double px, double py, double ax, double ay, double bx,
                                     double by) {
        const double abx = bx - ax;
        const double aby = by - ay;
        const double t = std::clamp(
            ((px - ax) * abx + (py - ay) * aby) / std::max(1e-9, abx * abx + aby * aby), 0.0, 1.0);
        const double dx = px - (ax + t * abx);
        const double dy = py - (ay + t * aby);
        return std::sqrt(dx * dx + dy * dy);
    };
    std::fill(image_.rgba.begin(), image_.rgba.end(), uint8_t{0});
    for (int x = 0; x < width_; ++x) {
        for (int channel = 0; channel < 3; ++channel) {
            const double* channel_tops = tops.data() + static_cast<std::size_t>(channel) * width_;
            // Red on top, then green, then blue, when split.
            const int band_bottom = split ? (channel + 1) * band_height : height_;
            const double top = channel_tops[x];
            if (top >= band_bottom) continue;
            const double left = channel_tops[std::max(0, x - 1)];
            const double right = channel_tops[std::min(width_ - 1, x + 1)];
            const double lowest = std::min({top, left, right});
            const double highest = std::max({top, left, right});
            // The fill's top edge fades over a few rows rather than one:
            // the pane can magnify the plot, and a single-pixel edge
            // would come back as a ladder on the slopes.
            constexpr double kFeather = 2.5;
            // The stroke: a line of this half-width traced along the
            // curve through the neighboring columns' tops, rendered by
            // perpendicular distance so steep segments stay as smooth
            // and as thick as flat ones.
            constexpr double kStrokeHalfWidth = 1.1;
            const int first_touched =
                std::max(band_bottom - band_height,
                         static_cast<int>(std::floor(lowest - kStrokeHalfWidth - 1.5)));
            const int stroke_last = static_cast<int>(std::ceil(highest + kStrokeHalfWidth + 1.5));
            for (int row = first_touched; row < band_bottom; ++row) {
                const double coverage = std::clamp((row + 1.0 - top) / kFeather, 0.0, 1.0);
                double stroke = 0.0;
                if (row <= stroke_last) {
                    const double px = x + 0.5;
                    const double py = row + 0.5;
                    const double distance =
                        std::min(segment_distance(px, py, x - 0.5, left, x + 0.5, top),
                                 segment_distance(px, py, x + 0.5, top, x + 1.5, right));
                    stroke = std::clamp(kStrokeHalfWidth + 0.75 - distance, 0.0, 1.0);
                }
                const double value = std::max(kFillValue * coverage, kOutlineValue * stroke);
                if (value <= 0.0) continue;
                uint8_t* pixel =
                    &image_.rgba[(static_cast<std::size_t>(row) * width_ + x) * 4 + channel];
                *pixel = static_cast<uint8_t>(std::max<double>(*pixel, value));
            }
        }
        for (int row = 0; row < height_; ++row)
            image_.rgba[(static_cast<std::size_t>(row) * width_ + x) * 4 + 3] = 255;
    }
    ++image_.sequence;
}

}  // namespace sidescopes
