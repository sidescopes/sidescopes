#pragma once

#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

struct HistogramSettings {
    // Trace gain applied to normalized densities before log mapping.
    float gain = 1.0f;
    // Sample every Nth pixel horizontally and vertically (1..8).
    int sampling_stride = 1;
};

// Classic RGB histogram: one column per 8-bit value, bar heights on the same
// max-normalized log curve as the other scopes, channels drawn additively so
// overlaps read as secondary colors. Densities are per-sample, keeping bar
// heights stable across sampling strides and region sizes.
//
// Not thread-safe; a single analysis thread owns each instance.
class Histogram {
public:
    static constexpr int kBins = 256;
    static constexpr int kHeight = 256;

    Histogram();

    void Configure(const HistogramSettings& settings);
    void Accumulate(const FrameView& frame, IntRect region);
    [[nodiscard]] const ScopeImage& Image() const { return image_; }

private:
    void MapBinsToImage(uint64_t sample_count);

    HistogramSettings settings_;
    // Three planes of kBins counts: red, green, blue.
    std::vector<uint32_t> bins_;
    ScopeImage image_;
};

}  // namespace sidescopes
