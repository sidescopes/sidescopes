#pragma once

#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

enum class HistogramStyle
{
    /// All three channels overlaid additively in one plot, the photo
    /// editors' habit: secondary colors encode channel overlap.
    Combined,
    /// Three stacked bands, one channel each: nothing occludes, exact
    /// per-channel shapes - and the default, because it reads like the
    /// parade and keeps the scope family consistent.
    PerChannel,
};

struct HistogramSettings
{
    HistogramStyle style = HistogramStyle::PerChannel;
    /// Sample every Nth pixel horizontally and vertically (1..8).
    int samplingStride = 1;
    /// Display image resolution. Sized near the pane, the plot renders
    /// close to one texture pixel per screen pixel, so the outline keeps
    /// one width on flats and steep slopes alike - a fixed texture
    /// stretched anisotropically turned the stroke elliptical.
    int imageWidth = 2048;
    int imageHeight = 768;
};

/// Classic RGB histogram, the way photo editors draw it: one column per
/// 8-bit value, square-root bar heights normalized to the tallest bin -
/// linear demands per-image zoom the moment one tone dominates, a log
/// curve melts every spike into a plateau, and the square root is the
/// editors' compromise that keeps spikes spiky and sparse tails visible
/// with nothing to tune - smoothed against bin-to-bin comb, channels
/// drawn additively so overlaps read as secondary colors. Heights stay
/// independent of sampling stride and region size.
///
/// Not thread-safe; a single analysis thread owns each instance.
class Histogram
{
public:
    static constexpr int Bins = 256;
    /// Defaults: supersampled horizontally - several image columns per
    /// bin, heights following a Catmull-Rom spline through the bin
    /// centers - so the histogram reads as a plotted function rather
    /// than a bar chart, without kinks at the bins.
    static constexpr int ImageWidth = Bins * 8;
    static constexpr int Height = 768;

    Histogram();

    /// Applies @p settings, clamping each value to its documented range.
    void configure(const HistogramSettings& settings);

    /// Folds a frame region into the bins.
    void accumulate(const FrameView& frame, IntRect region);

    /// The composed scope image.
    [[nodiscard]] const ScopeImage& image() const
    {
        return m_image;
    }

    /// The curve itself, three channels of Bins normalized heights in
    /// [0, 1] (full scale regardless of style). The interface draws it as
    /// a display-resolution line over the image: a stroke baked into the
    /// texture changes apparent thickness with the pane's stretch, which
    /// is anisotropic - thick on flats, thin on slopes, at any texture
    /// size.
    [[nodiscard]] const std::vector<float>& outlineHeights() const
    {
        return m_outline;
    }

private:
    void scatterRows(const FrameView& frame, IntRect region, int rowBegin, int rowEnd, uint32_t* bins) const;
    void mapBinsToImage();
    [[nodiscard]] std::vector<double> computeHeights() const;
    void exportOutline(const std::vector<double>& heights);
    void renderFill(const std::vector<double>& heights);

    void resize(int width, int height);

    HistogramSettings m_settings;
    int m_width = ImageWidth;
    int m_height = Height;
    // Three planes of Bins counts: red, green, blue.
    std::vector<uint32_t> m_bins;
    // Per-chunk private bin sets for the parallel accumulate, merged into
    // m_bins by integer addition.
    std::vector<uint32_t> m_threadBins;
    std::vector<float> m_outline;
    ScopeImage m_image;
};

}  // namespace sidescopes
