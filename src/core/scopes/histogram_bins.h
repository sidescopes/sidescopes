#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/scopes/chunk_scratch.h"
#include "core/scopes/sampling.h"

namespace sidescopes {

/// Codes one channel is binned into. An 8-bit code IS its bin, and a deeper
/// frame is rounded to the same 256: the bins are the axis, so nothing about
/// this follows the pane or the region.
inline constexpr int HistogramBinsPerChannel = 256;

/// Everything one scatter's result depends on. Two passes with equal keys write
/// bit-identical bins, which is what lets the second read the first's instead of
/// repeating the work.
///
/// The bin layout itself is not in here because it is not a variable: three
/// planes of HistogramBinsPerChannel counts, whatever the pane, the style or
/// the scope. That is the whole reason two histograms can share one set.
struct HistogramScatterKey
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
    /// Which pixels of it are read. Two histograms can be given different
    /// sampling strides - each carries its own - so this genuinely varies
    /// between two scopes over one frame.
    SampleGrid grid;

    [[nodiscard]] bool operator==(const HistogramScatterKey&) const = default;
};

/// @brief The bins one histogram pass scatters a region into, and the pass that
///        fills them.
///
/// Held apart from the engine because the bins are the expensive half and the
/// only half several scopes agree on. The bin layout is three planes of
/// HistogramBinsPerChannel counts and depends on nothing an engine holds, so
/// two histogram scopes over one region bin it identically whatever their
/// styles or their panes; everything after the scatter - the smoothing, the
/// heights, the fill - genuinely differs.
///
/// Not thread-safe. Scopes accumulate strictly one at a time on the analysis
/// thread, which is what makes sharing one of these between them safe without a
/// lock; nothing else may touch it.
class HistogramBins
{
public:
    /// Three planes of counts: red, green, blue.
    static constexpr std::size_t Total = static_cast<std::size_t>(HistogramBinsPerChannel) * 3;

    /// Folds the sampled rows of @p region into the bins, unless they already
    /// hold exactly this scatter - which is the whole point of sharing one of
    /// these: the second scope of a pass finds its answer already computed.
    void scatter(const FrameView& frame, IntRect region, const SampleGrid& grid);

    /// How many passes have really scattered, against how many asked. The
    /// sharing's own evidence: two histogram scopes over one frame must move
    /// this by one, not by two.
    [[nodiscard]] uint64_t scatters() const
    {
        return m_scatters;
    }

    /// The bins, laid out as three planes of HistogramBinsPerChannel counts.
    [[nodiscard]] const uint32_t* data() const
    {
        return m_bins.data();
    }

    [[nodiscard]] std::size_t size() const
    {
        return m_bins.size();
    }

    /// Takes the room a split pass needs for its per-chunk bins from the
    /// host's shared arena rather than from its own. The whole stack then
    /// holds one arena instead of a set each; bins never told keep their own,
    /// which is what a test or a benchmark does.
    void lendScratch(ChunkScratch::Lender lender, const void* context)
    {
        m_scratch.lendFrom(lender, context);
    }

private:
    /// The scatter itself, split across as many chunks as the rows justify.
    void scatterInto(const FrameView& frame, IntRect region, const SampleGrid& grid);
    /// Folds sampled rows [@p rowBegin, @p rowEnd) of @p region into @p bins.
    /// The bin layout is fixed at three planes, so this depends on nothing an
    /// instance holds.
    static void scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                            uint32_t* bins);
    /// The same, compiled for one pixel layout.
    template <typename Pixels>
    static void scatterRowsAs(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                              uint32_t* bins);

    // What the bins currently hold, so a second scope asking for the same
    // thing is answered rather than served again.
    HistogramScatterKey m_key;
    uint64_t m_scatters = 0;
    // Three planes of counts: red, green, blue.
    std::vector<uint32_t> m_bins = std::vector<uint32_t>(Total, 0);
    // Per-chunk private bin sets for the parallel scatter, merged into m_bins
    // by integer addition.
    ChunkScratch m_scratch;
};

}  // namespace sidescopes
