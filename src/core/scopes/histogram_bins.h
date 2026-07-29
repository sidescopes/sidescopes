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

/// @brief The bins one histogram pass scatters a region into, and the pass that
///        fills them.
///
/// Held apart from the engine because the bins are the expensive half of a
/// pass: the scatter walks every sampled pixel, while the smoothing, the
/// heights and the fill work over three planes of HistogramBinsPerChannel
/// counts whatever the pane or the style.
///
/// Not thread-safe; a single analysis thread owns each set.
class HistogramBins
{
public:
    /// Three planes of counts: red, green, blue.
    static constexpr std::size_t Total = static_cast<std::size_t>(HistogramBinsPerChannel) * 3;

    /// Folds the sampled rows of @p region into the bins.
    void scatter(const FrameView& frame, IntRect region, const SampleGrid& grid);

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
    /// Folds sampled rows [@p rowBegin, @p rowEnd) of @p region into @p bins.
    /// The bin layout is fixed at three planes, so this depends on nothing an
    /// instance holds.
    static void scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                            uint32_t* bins);
    /// The same, compiled for one pixel layout.
    template <typename Pixels>
    static void scatterRowsAs(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                              uint32_t* bins);

    // Three planes of counts: red, green, blue.
    std::vector<uint32_t> m_bins = std::vector<uint32_t>(Total, 0);
    // Per-chunk private bin sets for the parallel scatter, merged into m_bins
    // by integer addition.
    ChunkScratch m_scratch;
};

}  // namespace sidescopes
