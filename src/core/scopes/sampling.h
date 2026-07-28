#pragma once

#include "core/frame.h"

namespace sidescopes {

/// The ceiling on one accumulate pass, whatever its bins would justify. It
/// sits just above half a 4K frame, so a display up to 1440p is sampled row
/// for row. Every scope here derives its budget from its own bin count and
/// stays under this; it bounds one whose bins grow faster than that.
///
/// Measured over a whole 3456x2234 display against sampling every row, on
/// gradient-plus-grain content: the vectorscope image moves by at most 7 of
/// 255 with a mean of 0.09, and the histogram's mean is 0.08 with its rare
/// large deltas confined to the plotted curve's own edge - the curve moves by
/// well under a pixel.
inline constexpr long long SampleBudget = 4'200'000;

/// No thinning: every row in the region is sampled, whatever the region costs.
/// Reserved for callers that must see the whole region by construction - the
/// tests that establish a reference image, and any scope whose bins outnumber
/// the pixels available to fill them.
inline constexpr long long UnlimitedSamples = 0;

/// The budget for a scope holding @p binCount bins that needs
/// @p minimumSamplesPerBin samples in each of them to draw without visible
/// noise. Samples per bin is the currency, because it - not the region size - is
/// what decides whether a scope looks noisy: a whole display puts ten thousand
/// samples in every histogram bin and fifteen in every waveform bin, so one
/// global budget over-serves the first by an order of magnitude while
/// starving the second.
///
/// This is also what makes a small scope cheap. The histogram's bins are fixed,
/// so its cost stops growing with the pane; the waveform's scale with its own,
/// so a small one thins where a large one does not - cost proportional to the
/// scope first and the region second.
[[nodiscard]] inline long long budgetForBins(long long binCount, int minimumSamplesPerBin)
{
    return binCount * minimumSamplesPerBin;
}

/// The grid an accumulate pass samples a region on: every @p columnStride
/// pixel of every @p rowStride row, starting at the region's top-left corner.
struct SampleGrid
{
    int rowStride = 1;
    int columnStride = 1;
    /// Rows the pass visits, and samples it takes from each.
    int rows = 0;
    int columnsPerRow = 0;

    [[nodiscard]] bool operator==(const SampleGrid&) const = default;
};

/// The grid for @p region at the caller's @p requestedStride, which is the
/// user's own setting and is honoured on both axes as a lower bound. Rows are
/// thinned further, up to the same 1..8 range, until the pass stays inside
/// @p budget; UnlimitedSamples thins nothing.
///
/// Rows rather than columns: a waveform column is a place in the image, and
/// dropping source columns leaves image columns unevenly fed - the density
/// comb the fractional splat exists to prevent. A row is not a place in any
/// scope's output, only one more sample of the same distribution, and every
/// engine already normalizes by the sampled-row count it was given.
///
/// Thinning subsamples, it never averages: every value a scope plots is a
/// pixel that is really on screen. A box filter would be cheaper still and is
/// the wrong answer for a measurement tool - it would put colours on the
/// vectorscope that appear nowhere in the image.
[[nodiscard]] SampleGrid sampleGridFor(int requestedStride, IntRect region, long long budget);

/// The frame row the @p index-th sampled row of @p region comes from.
///
/// The phase walks with the index rather than staying at zero, so a stride of
/// two takes rows 0, 3, 4, 7, 8 and a stride of three cycles all three
/// residues. A fixed phase resonates with periodic content - and screen
/// content is full of it, from dithering to alternating table rows - and would
/// measure only one side of a pattern the display really shows. Measured on
/// content banded at every second and every third row, the staggered phase
/// leaves the vectorscope image a mean of 0.15 of 255 from sampling every row,
/// and the histogram 0.06, where a fixed phase left them 7.98 and 10.89.
///
/// The last few indices can run past the region with the phase applied; those
/// fall back to the unstaggered row, which is always inside it.
[[nodiscard]] inline int sampleRowOf(const SampleGrid& grid, IntRect region, int index)
{
    const int plain = index * grid.rowStride;
    const int staggered = plain + index % grid.rowStride;

    return region.y + (staggered < region.height ? staggered : plain);
}

}  // namespace sidescopes
