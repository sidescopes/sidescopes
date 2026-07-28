#pragma once

#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/scopes/chunk_scratch.h"
#include "core/scopes/histogram_bins.h"
#include "core/scopes/sampling.h"
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

/// Samples the histogram needs in each of its bins. Its bins are fixed at 768 -
/// 256 codes for each of three channels - however large the pane or the region,
/// so nothing about its precision grows with either, and a whole 3456x2234
/// display was putting ten thousand samples in every one of them. Calibrated
/// over that display against sampling every row: the image moves by a mean of
/// 0.16 of 255, its rare large deltas confined to the plotted curve's own edge,
/// and the pass falls from 4.6 ms to 3.3. A region small enough to fit inside
/// this budget is still sampled whole.
inline constexpr int HistogramMinSamplesPerBin = 1000;

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
    static constexpr int Bins = HistogramBinsPerChannel;
    /// Defaults: supersampled horizontally - several image columns per
    /// bin, heights following a Catmull-Rom spline through the bin
    /// centers - so the histogram reads as a plotted function rather
    /// than a bar chart, without kinks at the bins.
    static constexpr int ImageWidth = Bins * 8;
    static constexpr int Height = 768;

    /// Applies @p settings, clamping each value to its documented range.
    void configure(const HistogramSettings& settings);

    /// Folds a frame region into the bins.
    void accumulate(const FrameView& frame, IntRect region);

    /// Takes the room a split pass needs for its per-chunk bins from the
    /// host's shared arena rather than from its own. The whole stack then
    /// holds one arena instead of a set each; an engine never told keeps its
    /// own, which is what a test or a benchmark does.
    void lendScratch(ChunkScratch::Lender lender, const void* context)
    {
        bins().lendScratch(lender, context);
    }

    /// Scatters into @p bins rather than into its own, so several scopes over
    /// one region pay for that scatter once and the ones after the first find
    /// it already done. Null puts it back on its own, which is what an engine
    /// nobody has lent to - a test, a benchmark - always uses.
    ///
    /// The bins must outlive this engine, and only one thread may drive the
    /// engines sharing a set.
    void lendBins(HistogramBins* bins)
    {
        m_lentBins = bins;
    }

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
    /// The bins this pass reads: the lent set when there is one, otherwise the
    /// engine's own.
    [[nodiscard]] HistogramBins& bins()
    {
        return m_lentBins != nullptr ? *m_lentBins : m_ownBins;
    }

    [[nodiscard]] const HistogramBins& bins() const
    {
        return m_lentBins != nullptr ? *m_lentBins : m_ownBins;
    }

    void mapBinsToImage();
    [[nodiscard]] std::vector<double> computeHeights() const;
    void exportOutline(const std::vector<double>& heights);
    void renderFill(const std::vector<double>& heights);

    /// Brings the image up to the configured geometry, allocating it if this
    /// is the first pass. Nothing else may allocate it: an engine that has
    /// never accumulated holds no image at all.
    void ensureBuffers();
    void resize(int width, int height);

    HistogramSettings m_settings;
    int m_width = ImageWidth;
    int m_height = Height;
    // The bins this engine scatters into when nobody has lent it a set, and
    // the set it was lent. Both are never in use at once.
    HistogramBins m_ownBins;
    HistogramBins* m_lentBins = nullptr;
    std::vector<float> m_outline;
    ScopeImage m_image;
};

}  // namespace sidescopes
