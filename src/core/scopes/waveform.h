#pragma once

#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/scopes/sampling.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

inline constexpr int DefaultWaveformColumns = 1024;
inline constexpr int WaveformLevels = 256;

/// The widest and tallest image the waveform is computed at. Columns carry real
/// data - one per place in the region - so a wide pane deserves them, and this
/// is the step a scope filling a 4K display reaches. It stops there because the
/// cost is four private plane sets per parallel pass: 66 MB at this width
/// against 44 at the 2048 it used to stop at, and a wider one buys nothing any
/// current display can show. Height only resolves the level spline, the levels
/// themselves being fixed at 256 by eight-bit input, so it is worth far less
/// per megabyte and does not follow the pane.
inline constexpr int MaximumWaveformColumns = 3072;
inline constexpr int MaximumWaveformHeight = 768;

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
    [[nodiscard]] static NormalizedPoint project(const FloatColor& color);

private:
    void resize(int columns, int imageHeight);
    /// Folds sampled rows [@p rowBegin, @p rowEnd) into @p bins, which points
    /// at plane @p firstPlane of a plane set laid out like m_bins from there
    /// on. Only the planes the active mode draws are written.
    void scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                     uint32_t* bins, int firstPlane) const;
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
    // Per-chunk private plane sets for the parallel accumulate, merged into
    // m_bins by integer addition.
    std::vector<uint32_t> m_threadBins;
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
