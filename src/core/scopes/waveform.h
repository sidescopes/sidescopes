#pragma once

#include <optional>
#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

inline constexpr int kDefaultWaveformColumns = 1024;
inline constexpr int kWaveformLevels = 256;

struct WaveformSettings {
    // Trace gain applied to normalized bin densities before log mapping.
    float gain = 0.05f;
    // Sample every Nth pixel horizontally and vertically (1..8).
    int sampling_stride = 1;
    WaveformMode mode = WaveformMode::Rgb;
    // Horizontal resolution: more columns sharpen a big pane, fewer keep
    // a narrow region's columns densely populated - there is no point
    // resolving more columns than the region has pixels.
    int columns = kDefaultWaveformColumns;
    // Rendered image height. Level data always has 256 codes; a taller
    // image samples them through a spline so magnified traces draw as
    // curves rather than stretched texels.
    int image_height = kWaveformLevels;
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
    static constexpr int kColumns = kDefaultWaveformColumns;
    static constexpr int kLevels = kWaveformLevels;

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
    void Resize(int columns, int image_height);
    void MapBinsToImage(uint64_t sampled_rows);

    [[nodiscard]] std::size_t PlaneSize() const {
        return static_cast<std::size_t>(columns_) * kLevels;
    }

    WaveformSettings settings_;
    int columns_ = kDefaultWaveformColumns;
    int image_height_ = kWaveformLevels;
    // Planes: red, green, blue, luma — each columns x kLevels, a row per
    // level with level 255 in row zero.
    std::vector<uint32_t> bins_;
    // Parade scratch: per-channel window-maxed planes feeding the shared
    // composer.
    std::vector<uint32_t> parade_;
    // Per-plane scratch for the code-density correction: dead-code
    // reconstruction happens here before smoothing.
    std::vector<uint32_t> corrected_;
    std::vector<uint32_t> smoothed_;
    ScopeImage image_;
};

}  // namespace sidescopes
