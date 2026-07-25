#pragma once

#include "core/frame.h"

namespace sidescopes {

/// The most samples one accumulate pass takes from a region, for a scope whose
/// bins are densely populated. A whole high-resolution display puts a hundred
/// samples in every vectorscope bin and ten thousand in every histogram bin;
/// halving that changes nothing anyone can see, and the pixels behind the other
/// half are read thirty times a second. Everyday regions sit far below the
/// budget and are sampled whole: it sits just above half a 4K frame, so every
/// display up to 1440p is sampled row for row, a 4K or Retina one takes every
/// second row, and a 5K one every fourth.
///
/// Measured over a whole 3456x2234 display against sampling every row, on
/// gradient-plus-grain content: the vectorscope image moves by at most 7 of
/// 255 with a mean of 0.09, the neutral plane by 0.04, and the histogram's
/// mean is 0.08 with its rare large deltas confined to the plotted curve's own
/// edge - the curve moves by well under a pixel.
inline constexpr long long SampleBudget = 4'200'000;

/// No thinning: every row in the region is sampled. The waveform asks for this.
/// Its bins are an order of magnitude emptier than any other scope's - a
/// 2048-column trace over a whole display averages fifteen samples a bin, and
/// under nine per column per level - so it is already sample-starved and pays
/// for thinning in visible trace noise. Measured on the same content, halving
/// its rows moves the image by a mean of 5.4 of 255 with a fifth of the pixels
/// past 8, and 15.1 in the colored-luma style. That is not a trade the
/// measurement can afford.
inline constexpr long long UnlimitedSamples = 0;

/// The grid an accumulate pass samples a region on: every @p columnStride
/// pixel of every @p rowStride row, starting at the region's top-left corner.
struct SampleGrid
{
    int rowStride = 1;
    int columnStride = 1;
    /// Rows the pass visits, and samples it takes from each.
    int rows = 0;
    int columnsPerRow = 0;
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
