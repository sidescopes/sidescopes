#pragma once

#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

struct HistogramSettings {
    // Sample every Nth pixel horizontally and vertically (1..8).
    int sampling_stride = 1;
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
    // The plot is supersampled horizontally - eight image columns per
    // bin, heights following a Catmull-Rom spline through the bin centers,
    // top edge anti-aliased - so the histogram reads as a plotted
    // function rather than a bar chart, without kinks at the bins.
    static constexpr int kImageWidth = kBins * 8;
    // Tall enough that a full-window pane on a Retina display never
    // stretches the plot into visible softness.
    static constexpr int kHeight = 768;

    Histogram();

    void Configure(const HistogramSettings& settings);
    void Accumulate(const FrameView& frame, IntRect region);
    [[nodiscard]] const ScopeImage& Image() const { return image_; }

private:
    void MapBinsToImage();

    HistogramSettings settings_;
    // Three planes of kBins counts: red, green, blue.
    std::vector<uint32_t> bins_;
    ScopeImage image_;
};

}  // namespace sidescopes
