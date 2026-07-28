#include "core/scopes/histogram_bins.h"

#include <algorithm>
#include <cstddef>

#include "core/parallel_for.h"

namespace sidescopes {
namespace {

// Sampled rows below this per chunk keep the scatter single-threaded: a small
// region's pass finishes before spawned threads would pay off.
constexpr int AccumulateRowsPerChunk = 64;

}  // namespace

template <typename Pixels>
void HistogramBins::scatterRowsAs(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin,
                                  int rowEnd, uint32_t* bins)
{
    uint32_t* redBins = bins;
    uint32_t* greenBins = bins + HistogramBinsPerChannel;
    uint32_t* blueBins = bins + static_cast<std::ptrdiff_t>(2) * HistogramBinsPerChannel;

    // One count in the bin the code rounds to. An 8-bit code IS its bin, so
    // this is exactly the count it always was.
    //
    // A deeper frame gains nothing here and is not made to pretend otherwise:
    // the bins are the axis, and 256 of them cannot show what a tenth bit says.
    // Splitting each sample between the two bins it straddles was measured as
    // an alternative and rejected - it places the curve no better than rounding
    // does and widens every narrow peak, because a two-bin split is a kernel
    // one bin wide. More bins is the only thing that would help, and that is a
    // memory decision recorded in the notes rather than one taken here.
    const auto splat = [](uint32_t* plane, int code) { ++plane[levelIn<Pixels, WholeLevelBits>(code)]; };

    for (int i = rowBegin; i < rowEnd; ++i) {
        const int py = sampleRowOf(grid, region, i);
        const uint8_t* row = frame.rawPixelAt(region.x, py);
        for (int px = 0; px < region.width; px += grid.columnStride) {
            const Sample sample = Pixels::read(row + static_cast<std::size_t>(px) * 4);
            splat(blueBins, sample.b);
            splat(greenBins, sample.g);
            splat(redBins, sample.r);
        }
    }
}

void HistogramBins::scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin,
                                int rowEnd, uint32_t* bins)
{
    // Dispatched once per chunk, never per pixel: the inner loop is compiled
    // for one layout and holds no test of the format.
    if (frame.format == PixelFormat::Argb2101010) {
        scatterRowsAs<Argb2101010Pixels>(frame, region, grid, rowBegin, rowEnd, bins);
    } else {
        scatterRowsAs<Bgra8Pixels>(frame, region, grid, rowBegin, rowEnd, bins);
    }
}

void HistogramBins::scatter(const FrameView& frame, IntRect region, const SampleGrid& grid)
{
    const int rowCount = grid.rows;
    const int chunks = parallelChunkCount(rowCount, AccumulateRowsPerChunk);
    if (chunks <= 1) {
        std::fill(m_bins.begin(), m_bins.end(), 0u);
        scatterRows(frame, region, grid, 0, rowCount, m_bins.data());

        return;
    }

    // Each chunk owns a private bin set it clears and scatters into; integer
    // addition then merges them back to a bit-exact total.
    const std::size_t binCount = m_bins.size();
    uint32_t* threadBins = m_scratch.borrow(binCount * static_cast<std::size_t>(chunks));
    runParallelChunks(chunks, rowCount, [&](int chunk, int begin, int end) {
        uint32_t* bins = threadBins + static_cast<std::size_t>(chunk) * binCount;
        std::fill_n(bins, binCount, uint32_t{0});
        scatterRows(frame, region, grid, begin, end, bins);
    });
    mergeBins(threadBins, binCount, chunks, m_bins.data());
}

}  // namespace sidescopes
