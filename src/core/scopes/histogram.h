#pragma once

#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

enum class HistogramStyle {
    // All three channels overlaid additively in one plot, the photo
    // editors' habit: secondary colors encode channel overlap.
    Combined,
    // Three stacked bands, one channel each: nothing occludes, exact
    // per-channel shapes - and the default, because it reads like the
    // parade and keeps the scope family consistent.
    PerChannel,
};

struct HistogramSettings {
    HistogramStyle style = HistogramStyle::PerChannel;
    // Sample every Nth pixel horizontally and vertically (1..8).
    int sampling_stride = 1;
    // Display image resolution. Sized near the pane, the plot renders
    // close to one texture pixel per screen pixel, so the outline keeps
    // one width on flats and steep slopes alike - a fixed texture
    // stretched anisotropically turned the stroke elliptical.
    int image_width = 2048;
    int image_height = 768;
};

// Classic RGB histogram, the way photo editors draw it: one column per
// 8-bit value, square-root bar heights normalized to the tallest bin -
// linear demands per-image zoom the moment one tone dominates, a log
// curve melts every spike into a plateau, and the square root is the
// editors' compromise that keeps spikes spiky and sparse tails visible
// with nothing to tune - smoothed against bin-to-bin comb, channels
// drawn additively so overlaps read as secondary colors. Heights stay
// independent of sampling stride and region size.
//
// Not thread-safe; a single analysis thread owns each instance.
class Histogram {
public:
    static constexpr int kBins = 256;
    // Defaults: supersampled horizontally - several image columns per
    // bin, heights following a Catmull-Rom spline through the bin
    // centers - so the histogram reads as a plotted function rather
    // than a bar chart, without kinks at the bins.
    static constexpr int kImageWidth = kBins * 8;
    static constexpr int kHeight = 768;

    Histogram();

    void Configure(const HistogramSettings& settings);
    void Accumulate(const FrameView& frame, IntRect region);
    [[nodiscard]] const ScopeImage& Image() const { return image_; }

private:
    void MapBinsToImage();

    void Resize(int width, int height);

    HistogramSettings settings_;
    int width_ = kImageWidth;
    int height_ = kHeight;
    // Three planes of kBins counts: red, green, blue.
    std::vector<uint32_t> bins_;
    ScopeImage image_;
};

}  // namespace sidescopes
