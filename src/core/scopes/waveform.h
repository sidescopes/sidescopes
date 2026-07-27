#pragma once

#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/scopes/chunk_scratch.h"
#include "core/scopes/sampling.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

inline constexpr int DefaultWaveformColumns = 1024;

/// Levels the trace is binned into. Held at 256 by measurement, not by the
/// input: a ten-bit capture carries about two bits more than this resolves, so
/// unlike the columns this is now a deliberate ceiling rather than a floor the
/// source imposed.
///
/// Doubling it doubles the bins - 16 MB more at the default pane, 48 at the
/// widest - and costs 69% more per pass, against an application whose whole
/// footprint is around 137 MB. What it buys is confined to columns whose own
/// tonal spread is under about a level, since a column spanning two levels or
/// more is already resolved to within a few percent here. The image is capped
/// at MaximumWaveformHeight anyway, so beyond that many levels there is no
/// pixel left to draw one in. If it is ever raised, 512 is the only step worth
/// taking and it should follow the pane the way the columns do.
inline constexpr int WaveformLevels = 256;

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

/// Words of unused space between one bin plane and the next, so that the four
/// never start an exact power of two apart. See Waveform::planeSize.
inline constexpr std::size_t WaveformPlanePadding = 32;

/// Bins of unused space at the end of every level's row, so that one level's
/// row never starts an exact power of two from the next. See Waveform::rowPitch.
inline constexpr std::size_t WaveformRowPadding = 8;

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
        m_scratch.lendFrom(lender, context);
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
    /// Brings the planes up to the configured geometry, allocating them if
    /// this is the first pass. Nothing else may allocate them: an engine that
    /// has never accumulated holds no planes at all.
    void ensureBuffers();
    void resize(int columns, int imageHeight);
    /// Folds sampled rows [@p rowBegin, @p rowEnd) into @p bins, which points
    /// at plane @p firstPlane of a plane set laid out like m_bins from there
    /// on. Only the planes the active mode draws are written.
    void scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                     uint32_t* bins, int firstPlane) const;
    /// The same, compiled for one pixel layout.
    template <typename Pixels>
    void scatterRowsAs(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                       uint32_t* bins, int firstPlane) const;
    void mapBinsToImage(uint64_t sampledRows);
    void correctBinDensities();
    void buildParade(const uint32_t* redPlane, const uint32_t* greenPlane, const uint32_t* bluePlane);
    void composeImage(const uint32_t* redPlane, const uint32_t* greenPlane, const uint32_t* bluePlane,
                      const uint32_t* lumaPlane, double gain, double intensityScale);

    /// The distance from one level's row to the next, in bins. Deliberately
    /// NOT the column count.
    ///
    /// With the two equal, the rows of a plane sit `columns x 4` bytes apart -
    /// an exact power of two at 2048 columns, which the detail ladder
    /// deliberately selects for any waveform pane 1434 to 2867 pixels wide. An
    /// RGB scatter writes three or four planes per sample at three or four
    /// different levels, so those rows then contend for the same cache sets.
    /// Measured on an M5 Pro over a 3024x1964 region: the RGB pass cost 15.57
    /// ms at 2048 columns against 12.16 at both 2047 and 2049, and the parade
    /// 13.20 against 9.84 and 9.66. Luma, which writes one plane, was flat at
    /// 6.4 to 6.7 across all three - the tell that it is the multi-plane
    /// scatter and not the width itself.
    ///
    /// The padding is never written and never read as data. Nothing may
    /// iterate a row by rowPitch: every pass over one stops at m_columns.
    [[nodiscard]] std::size_t rowPitch() const
    {
        return static_cast<std::size_t>(m_columns) + WaveformRowPadding;
    }

    /// The bins one plane really holds: a row per level, a row pitch apart.
    [[nodiscard]] std::size_t binsPerPlane() const
    {
        return rowPitch() * Levels;
    }

    /// The distance from one plane to the next, which is its bins plus a pad.
    ///
    /// Without the pad the four planes sit an exact power of two apart at
    /// every column count the application asks for, and an RGB scatter - which
    /// writes all four per sample - then has all four land in the same cache
    /// sets. Measured on an M5 Pro over a 3024x1964 region, the pass took
    /// 17.2 ms at 1024 columns and 28.0 at 2048, against 12.5 at 2049: one
    /// extra column was 55% faster than the power of two beside it. With the
    /// pad they are 7.7 and 14.2. Luma, which writes one plane, never showed
    /// it. Swept at 0, 8, 16, 32, 64, 128 and 512 words; 32 is where it
    /// flattens, and it is a cache line on the machines this runs on.
    ///
    /// The padding is never written and never read as data. Nothing may
    /// iterate a plane by planeSize: every pass over one walks its levels and
    /// columns, which stops at the bins the plane really holds.
    [[nodiscard]] std::size_t planeSize() const
    {
        return binsPerPlane() + WaveformPlanePadding;
    }

    WaveformSettings m_settings;
    int m_columns = DefaultWaveformColumns;
    int m_imageHeight = WaveformLevels;
    // Planes: red, green, blue, luma — each columns x Levels, a row per
    // level with level 255 in row zero.
    std::vector<uint32_t> m_bins;
    // Per-chunk private plane sets for the parallel accumulate, merged into
    // m_bins by integer addition.
    ChunkScratch m_scratch;
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
