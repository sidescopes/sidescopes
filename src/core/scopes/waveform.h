#pragma once

#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

inline constexpr int DefaultWaveformColumns = 1024;
inline constexpr int WaveformLevels = 256;

struct WaveformSettings
{
    /// Trace gain applied to normalized bin densities before log mapping.
    float gain = 0.05f;
    /// Sample every Nth pixel horizontally and vertically (1..8).
    int samplingStride = 1;
    WaveformMode mode = WaveformMode::Rgb;
    /// Horizontal resolution: more columns sharpen a big pane, fewer keep
    /// a narrow region's columns densely populated - there is no point
    /// resolving more columns than the region has pixels.
    int columns = DefaultWaveformColumns;
    /// Rendered image height. Level data always has 256 codes; a taller
    /// image samples them through a spline so magnified traces draw as
    /// curves rather than stretched texels.
    int imageHeight = WaveformLevels;
};

/// Waveform monitor: level (vertical) against image column (horizontal).
/// Depending on the mode it plots Rec.709 luma, the three channels as
/// colored overlaid traces, or both. Density mapping follows the same log
/// rules as the vectorscope, but normalization is per sampled row: a column
/// receives one sample per row, so this keeps column brightness invariant to
/// both the sampling stride and the region size.
///
/// Not thread-safe; a single analysis thread owns each instance.
class Waveform
{
public:
    static constexpr int Columns = DefaultWaveformColumns;
    static constexpr int Levels = WaveformLevels;

    Waveform();

    /// Applies @p settings, clamping each value to its documented range.
    void configure(const WaveformSettings& settings);

    /// Folds a frame region into the bins.
    void accumulate(const FrameView& frame, IntRect region);

    /// The composed scope image.
    [[nodiscard]] const ScopeImage& image() const
    {
        return m_image;
    }

    /// The luma level a color sits at, as a normalized vertical position.
    /// The horizontal position depends on where the color appears in the
    /// image, which a bare color cannot know; x is reported as -1 and callers
    /// draw a horizontal level line. Per-channel lines are trivial for
    /// callers to place themselves: the level of a channel is its own value.
    [[nodiscard]] NormalizedPoint project(const FloatColor& color) const;

private:
    void resize(int columns, int imageHeight);
    void mapBinsToImage(uint64_t sampledRows);
    void correctBinDensities();
    void buildParade(const uint32_t* redPlane, const uint32_t* greenPlane, const uint32_t* bluePlane);
    void composeImage(const uint32_t* redPlane, const uint32_t* greenPlane, const uint32_t* bluePlane,
                      const uint32_t* lumaPlane, double gain, double intensityScale);

    [[nodiscard]] std::size_t planeSize() const
    {
        return static_cast<std::size_t>(m_columns) * Levels;
    }

    WaveformSettings m_settings;
    int m_columns = DefaultWaveformColumns;
    int m_imageHeight = WaveformLevels;
    // Planes: red, green, blue, luma — each columns x Levels, a row per
    // level with level 255 in row zero.
    std::vector<uint32_t> m_bins;
    // Parade scratch: per-channel window-maxed planes feeding the shared
    // composer.
    std::vector<uint32_t> m_parade;
    // Per-plane scratch for the code-density correction: dead-code
    // reconstruction happens here before smoothing.
    std::vector<uint32_t> m_corrected;
    std::vector<uint32_t> m_smoothed;
    ScopeImage m_image;
};

}  // namespace sidescopes
