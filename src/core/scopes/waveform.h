#pragma once

#include <optional>
#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

struct WaveformSettings {
    // Trace gain applied to normalized bin densities before log mapping.
    float gain = 0.05f;
    // Sample every Nth pixel horizontally and vertically (1..8).
    int sampling_stride = 1;
    WaveformMode mode = WaveformMode::Rgb;
};

// Waveform monitor: level (vertical) against image column (horizontal).
// Depending on the mode it plots Rec.709 luma, the three channels as
// colored overlaid traces, or both. Density mapping follows the same log
// rules as the vectorscope, but normalization is per sampled row: a column
// receives one sample per row, so this keeps column brightness invariant to
// both the sampling stride and the region size.
//
// Not thread-safe; a single analysis thread owns each instance.
class Waveform {
public:
    static constexpr int kColumns = 1024;
    static constexpr int kLevels = 256;

    Waveform();

    void Configure(const WaveformSettings& settings);
    void Accumulate(const FrameView& frame, IntRect region);
    [[nodiscard]] const ScopeImage& Image() const { return image_; }

    // The luma level a color sits at, as a normalized vertical position.
    // The horizontal position depends on where the color appears in the
    // image, which a bare color cannot know; x is reported as -1 and callers
    // draw a horizontal level line. Per-channel lines are trivial for
    // callers to place themselves: the level of a channel is its own value.
    [[nodiscard]] std::optional<NormalizedPoint> Project(const FloatColor& color) const;

private:
    void MapBinsToImage(uint64_t sampled_rows);

    static constexpr std::size_t kPlaneSize = static_cast<std::size_t>(kColumns) * kLevels;

    WaveformSettings settings_;
    // Planes: red, green, blue, luma — each kColumns x kLevels, a row per
    // level with level 255 in row zero.
    std::vector<uint32_t> bins_;
    // Per-plane scratch for the code-density correction: dead-code
    // reconstruction happens here before smoothing.
    std::vector<uint32_t> corrected_;
    std::vector<uint32_t> smoothed_;
    ScopeImage image_;
};

}  // namespace sidescopes
