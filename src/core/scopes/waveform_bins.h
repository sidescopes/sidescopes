#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/scopes/chunk_scratch.h"
#include "core/scopes/sampling.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

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

/// Words of unused space between one bin plane and the next, so that the four
/// never start an exact power of two apart. See WaveformBins::planeSize.
inline constexpr std::size_t WaveformPlanePadding = 32;

/// Bins of unused space at the end of every level's row, so that one level's
/// row never starts an exact power of two from the next. See
/// WaveformBins::rowPitch.
inline constexpr std::size_t WaveformRowPadding = 8;

/// Which planes a mode draws: the RGB channels, the luma trace, and whether
/// luma carries the source color. Derived once and threaded through
/// accumulation, correction, and composition so the three stay in step.
struct WaveformModeFlags
{
    bool rgb;
    bool luma;
    bool coloredLuma;
};

[[nodiscard]] WaveformModeFlags waveformModeFlags(WaveformMode mode);

/// The planes a mode actually populates, as one contiguous range of the four.
/// The three channel planes lead and luma trails, so every mode's working set
/// is a range rather than a scattered set.
struct WaveformPlaneSpan
{
    int first;
    int count;

    [[nodiscard]] bool operator==(const WaveformPlaneSpan&) const = default;
};

/// Clearing, scattering, merging and correcting all four planes regardless of
/// mode costs a plane's worth of bin traffic per frame for nothing: at a wide
/// pane that is megabytes, and the plain RGB mode - the default - never draws
/// the luma plane at all. The colored-luma mode tints from the channel planes,
/// so it needs them even though it draws no RGB trace.
[[nodiscard]] WaveformPlaneSpan waveformPlaneSpan(const WaveformModeFlags& flags);

/// The smallest range covering both. The four planes are few enough that every
/// union this produces is contiguous, and a range wider than the union would
/// still be correct - it writes planes nobody reads rather than leaving one
/// unwritten.
[[nodiscard]] WaveformPlaneSpan waveformSpanUnion(const WaveformPlaneSpan& a, const WaveformPlaneSpan& b);

/// The mode a pass must run in to fill @p span, given that @p requested is what
/// some scope asked for.
///
/// This is the job WaveformMode::RgbAndLuma exists for now. It is not a mode
/// anybody selects - the RGB and luma waveforms are separate scopes, and
/// stacking them IS asking for both - it is what ONE pass runs in when both
/// are on screen, so that the pixels are walked once instead of twice.
[[nodiscard]] WaveformMode waveformModeForSpan(const WaveformPlaneSpan& span, WaveformMode requested);

/// Everything one scatter's result depends on. Two passes with equal keys write
/// bit-identical bins, which is what lets the second read the first's instead
/// of repeating the work.
struct WaveformScatterKey
{
    /// The frame, by the identity that decides its content: where the pixels
    /// are, how they are laid out, and which frame they came from. The sequence
    /// alone would trust a producer never to reuse a number and the pointer
    /// alone would trust it never to reuse a buffer, so both are here.
    const uint8_t* pixels = nullptr;
    int strideBytes = 0;
    uint64_t sequence = 0;
    PixelFormat format = PixelFormat::Bgra8;
    /// The region already clamped to the frame, which is what the scatter walks.
    IntRect region;
    SampleGrid grid;
    int columns = 0;
    /// The planes the pass writes. It belongs in the key: a mode writing only
    /// the luma plane leaves the channel planes holding an OLDER FRAME, so a
    /// scope wanting those must not read this pass as if it had filled them.
    WaveformPlaneSpan span{0, 0};
    /// The colored-luma scatter puts value-weighted mass at the luma level
    /// where every other mode puts counts at each channel's own, so it fills the
    /// same planes with different numbers and can never be read as theirs.
    bool coloredLuma = false;

    [[nodiscard]] bool operator==(const WaveformScatterKey&) const = default;
};

/// @brief The bins one waveform pass scatters a region into, and the pass that
///        fills them.
///
/// Held apart from the engine because the bins are the expensive half and the
/// only half several scopes agree on. The waveform and the RGB parade are the
/// same module over the same region at the same geometry, so their scatters
/// agree bin for bin and everything after the scatter genuinely differs. One of
/// these shared between them pays for that scatter once.
///
/// THE GEOMETRY HAS TO BE ONE GEOMETRY, and the host is what makes it so: it
/// measures every waveform-family pane and applies the larger as one image size
/// to all of them. Scopes sharing a set at DIFFERENT column counts would still
/// draw correctly - each pass re-lays the bins for what it asked - but they
/// would reallocate megabytes at every scope of every frame. Any new member of
/// the family must be given the family's size, not its own.
///
/// Not thread-safe. Scopes accumulate strictly one at a time on the analysis
/// thread, which is what makes sharing one of these between them safe without
/// a lock; nothing else may touch it.
class WaveformBins
{
public:
    /// Folds sampled rows of @p region into the bins, unless they already hold
    /// exactly this scatter - which is the whole point of sharing one of these:
    /// the second scope of a pass finds its answer already computed.
    ///
    /// Only the planes @p mode draws are cleared and written; the rest keep
    /// whatever an earlier pass left, and the span in the key is what stops
    /// anybody reading one of those as if this pass had filled it.
    void scatter(const FrameView& frame, IntRect region, const SampleGrid& grid, WaveformMode mode, int columns);

    /// How many passes have really scattered, against how many asked. The
    /// sharing's own evidence: a stack of waveform-family scopes over one frame
    /// must move this by one, not by its size.
    [[nodiscard]] uint64_t scatters() const
    {
        return m_scatters;
    }

    /// The planes the last pass really wrote, which is what any of them may be
    /// read as an answer for. Empty before the first scatter.
    [[nodiscard]] WaveformPlaneSpan writtenSpan() const
    {
        return m_key.span;
    }

    /// The bins, laid out as four planes a planeSize() apart. Null before the
    /// first scatter: an engine that has never accumulated holds none.
    [[nodiscard]] const uint32_t* data() const
    {
        return m_bins.data();
    }

    [[nodiscard]] std::size_t size() const
    {
        return m_bins.size();
    }

    /// The columns the bins are laid out for, 0 before the first scatter.
    [[nodiscard]] int columns() const
    {
        return m_columns;
    }

    /// Takes the room a split pass needs for its per-chunk bins from the
    /// host's shared arena rather than from its own. The whole stack then
    /// holds one arena instead of a set each; bins never told keep their own,
    /// which is what a test or a benchmark does.
    void lendScratch(ChunkScratch::Lender lender, const void* context)
    {
        m_scratch.lendFrom(lender, context);
    }

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
    /// iterate a row by rowPitch: every pass over one stops at the columns.
    [[nodiscard]] std::size_t rowPitch() const
    {
        return static_cast<std::size_t>(m_columns) + WaveformRowPadding;
    }

    /// The bins one plane really holds: a row per level, a row pitch apart.
    [[nodiscard]] std::size_t binsPerPlane() const
    {
        return rowPitch() * WaveformLevels;
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

private:
    /// Brings the bins up to @p columns, allocating them if this is the first
    /// pass. Nothing else may allocate them.
    void ensureBuffers(int columns);
    /// The scatter itself, split across as many chunks as the rows justify.
    void scatterInto(const FrameView& frame, IntRect region, const SampleGrid& grid, WaveformMode mode,
                     const WaveformPlaneSpan& span);
    /// Folds sampled rows [@p rowBegin, @p rowEnd) into @p bins, which points
    /// at plane @p firstPlane of a plane set laid out like m_bins from there
    /// on. Only the planes the active mode draws are written.
    void scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                     uint32_t* bins, int firstPlane, WaveformMode mode) const;
    /// The same, compiled for one pixel layout.
    template <typename Pixels>
    void scatterRowsAs(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                       uint32_t* bins, int firstPlane, WaveformMode mode) const;

    /// The scatter this pass would be part of, ignoring which planes are asked
    /// for and how they are filled: everything two scopes of one frame agree on.
    /// A change in it is a new frame or a new region, which is what starts a
    /// fresh reckoning of what the family wants.
    [[nodiscard]] static WaveformScatterKey subjectOf(const WaveformScatterKey& key);
    /// The planes this pass writes: @p wanted, widened by what the family asked
    /// of these bins over the previous frame so the FIRST pass of a frame
    /// answers the ones after it.
    [[nodiscard]] WaveformPlaneSpan spanToScatter(const WaveformPlaneSpan& wanted) const;

    int m_columns = 0;
    // What the bins currently hold, so a second scope asking for the same
    // thing is answered rather than served again.
    WaveformScatterKey m_key;
    // The subject the wants below were gathered over, so a new frame or a moved
    // region starts a new generation of them.
    WaveformScatterKey m_subject;
    // The planes the plain-mode scopes asked for over the subject in the bins,
    // and over the one before it. Predicting from the previous frame is what
    // lets ONE pass answer a stack the pass cannot see: it costs a wider pass
    // for one frame after a scope appears, and decays on its own once one stops
    // asking, so a stack that settles pays exactly what it uses. A colored-luma
    // request is left out: it fills the same planes with different numbers, so
    // nothing another mode writes could answer it and widening a plain pass on
    // its account would buy a plane of traffic for nobody.
    WaveformPlaneSpan m_wanted{0, 0};
    WaveformPlaneSpan m_wantedBefore{0, 0};
    uint64_t m_scatters = 0;
    // Planes: red, green, blue, luma — each columns x WaveformLevels, a row
    // per level with level 255 in row zero.
    std::vector<uint32_t> m_bins;
    // Per-chunk private plane sets for the parallel scatter, merged into
    // m_bins by integer addition.
    ChunkScratch m_scratch;
};

}  // namespace sidescopes
