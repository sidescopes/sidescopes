#pragma once

#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/scopes/chunk_scratch.h"
#include "core/scopes/sampling.h"
#include "core/scopes/scope_types.h"
#include "core/scopes/waveform_bins.h"

namespace sidescopes {

inline constexpr int DefaultWaveformColumns = 1024;

/// The widest and tallest image the waveform is computed at. Columns carry real
/// data - one per place in the region - so a wide pane deserves them, and this
/// is the step a scope filling a 4K display reaches. It stops there because the
/// cost is four private plane sets per parallel pass: 66 MB at this width
/// against 44 at the 2048 it used to stop at, and a wider one buys nothing any
/// current display can show. Height only resolves the level spline, the levels
/// themselves being held at WaveformLevels, so it is worth far less per
/// megabyte and does not follow the pane.
inline constexpr int MaximumWaveformColumns = 3072;
inline constexpr int MaximumWaveformHeight = 768;

/// Samples the waveform needs in each of its bins. It is the sample-starved
/// scope - its bins are columns x 256, an order of magnitude more than any other
/// scope's, so a 2048-column trace over a whole display holds only fifteen
/// samples a bin - and its log-and-gamma mapping amplifies bin noise where the
/// vectorscope's density estimate suppresses it. So its minimum is the highest
/// here, and because its bins scale with its pane the budget it produces is
/// above a whole display's pixel count from the default thousand columns up: a
/// waveform large enough to need every row still gets every row, and this is the
/// value that guarantees it for a 4K region at that default rather than landing
/// a percent short of it.
///
/// Below that it thins, which is the deliberate trade for a small pane.
/// Calibrated over a whole 3456x2234 display at 512 columns: the pass falls from
/// 11.3 ms to 6.5, and the image moves by a mean of 1.1 of 255 on a photograph
/// and 1.9 on gradient-plus-grain - an order of magnitude more than the other
/// scopes pay, which is why it buys only the small-pane case.
inline constexpr int WaveformMinSamplesPerBin = 32;

/// What one column of one bin plane holds: its densest bin, the level that bin
/// sits at, and the mass of the whole column. Together they say whether the
/// column carries a distribution or a single flat tone, which is what decides
/// the trace's normalization ceiling.
struct ColumnDensity
{
    uint32_t peak = 0;
    uint64_t mass = 0;
    int level = 0;
};

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
    /// Divides the samples per bin the pass asks for (1..8, 1 = every one it
    /// would take). Host-driven and never a user setting: the columns are
    /// places in the region and must not be coarsened, so this is what a
    /// moving region gives up instead. Halving it costs a mean 1.3 of 255 for
    /// 36% of the pass, and does nothing at all where the budget already
    /// exceeds the region - which is every region under about four
    /// megapixels.
    int sampleThinning = 1;
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

    /// Takes the room a split pass needs for its per-chunk bins from the
    /// host's shared arena rather than from its own. The whole stack then
    /// holds one arena instead of a set each; an engine never told keeps its
    /// own, which is what a test or a benchmark does.
    void lendScratch(ChunkScratch::Lender lender, const void* context)
    {
        bins().lendScratch(lender, context);
    }

    /// Scatters into @p bins rather than into its own, so several scopes over
    /// one region at one geometry pay for that scatter once and the ones after
    /// the first find it already done. Null puts it back on its own, which is
    /// what an engine nobody has lent to - a test, a benchmark - always uses.
    ///
    /// The bins must outlive this engine, and only one thread may drive the
    /// engines sharing a set.
    void lendBins(WaveformBins* bins)
    {
        m_lentBins = bins;
    }

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
    /// Brings the image up to the configured geometry, allocating it if this
    /// is the first pass. Nothing else may allocate it: an engine that has
    /// never accumulated holds no image at all.
    void ensureBuffers();
    void resize(int columns, int imageHeight);
    void mapBinsToImage(uint64_t sampledRows);
    void correctBinDensities();
    void buildParade(const uint32_t* redPlane, const uint32_t* greenPlane, const uint32_t* bluePlane);
    void composeImage(const uint32_t* redPlane, const uint32_t* greenPlane, const uint32_t* bluePlane,
                      const uint32_t* lumaPlane, double gain, double intensityScale);

    /// The bins this pass reads: the lent set when there is one, otherwise the
    /// engine's own.
    [[nodiscard]] WaveformBins& bins()
    {
        return m_lentBins != nullptr ? *m_lentBins : m_ownBins;
    }

    [[nodiscard]] const WaveformBins& bins() const
    {
        return m_lentBins != nullptr ? *m_lentBins : m_ownBins;
    }

    /// The bin geometry the image is composed against, which is the bins' own.
    /// Named here so the composer reads one word rather than a chain.
    [[nodiscard]] std::size_t rowPitch() const
    {
        return bins().rowPitch();
    }

    [[nodiscard]] std::size_t planeSize() const
    {
        return bins().planeSize();
    }

    WaveformSettings m_settings;
    int m_columns = DefaultWaveformColumns;
    int m_imageHeight = WaveformLevels;
    // The bins this engine scatters into when nobody has lent it a set, and
    // the set it was lent. Both are never in use at once.
    WaveformBins m_ownBins;
    WaveformBins* m_lentBins = nullptr;
    // One plane's per-column densities, the evidence the normalization ceiling
    // is chosen from. Sized with the planes and rewritten per plane, so
    // choosing the ceiling allocates nothing.
    std::vector<ColumnDensity> m_columnDensities;
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
