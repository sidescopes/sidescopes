#include "core/scopes/waveform_bins.h"

#include <algorithm>

#include "core/parallel_for.h"

namespace sidescopes {
namespace {

// Below this many sampled rows per chunk the scatter stays single-threaded:
// the pass finishes before spawned threads would pay off.
constexpr int AccumulateRowsPerChunk = 64;

// Rec.709 luma weights, fixed-point x256, applied to display-encoded values.
inline int luma709(int r, int g, int b)
{
    return (54 * r + 183 * g + 19 * b) >> 8;
}

/// The planes the scatter writes, resolved against a plane set whose first
/// plane is @p firstPlane. A plane the mode does not draw stays null: the span
/// holds no storage for it, so no pointer to it is ever formed.
struct ScatterPlanes
{
    uint32_t* red = nullptr;
    uint32_t* green = nullptr;
    uint32_t* blue = nullptr;
    uint32_t* luma = nullptr;
};

ScatterPlanes scatterPlanesFor(const WaveformModeFlags& flags, uint32_t* bins, int firstPlane, std::size_t planeSize)
{
    const auto planeAt = [&](int index) { return bins + static_cast<std::size_t>(index - firstPlane) * planeSize; };
    ScatterPlanes planes;
    if (flags.rgb || flags.coloredLuma) {
        planes.red = planeAt(0);
        planes.green = planeAt(1);
        planes.blue = planeAt(2);
    }
    if (flags.luma) {
        planes.luma = planeAt(3);
    }

    return planes;
}

}  // namespace

WaveformModeFlags waveformModeFlags(WaveformMode mode)
{
    const bool coloredLuma = mode == WaveformMode::ColoredLuma;

    return WaveformModeFlags{
        mode != WaveformMode::Luma && !coloredLuma,
        mode == WaveformMode::Luma || mode == WaveformMode::RgbAndLuma || coloredLuma,
        coloredLuma,
    };
}

WaveformPlaneSpan waveformPlaneSpan(const WaveformModeFlags& flags)
{
    if (!flags.rgb && !flags.coloredLuma) {
        return WaveformPlaneSpan{3, 1};
    }

    return WaveformPlaneSpan{0, flags.luma ? 4 : 3};
}

void WaveformBins::ensureBuffers(int columns)
{
    if (!m_bins.empty() && m_columns == columns) {
        return;
    }
    m_columns = columns;
    m_bins.assign(planeSize() * 4, 0);
}

template <typename Pixels>
void WaveformBins::scatterRowsAs(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin,
                                 int rowEnd, uint32_t* bins, int firstPlane, WaveformMode mode) const
{
    const WaveformModeFlags flags = waveformModeFlags(mode);
    const ScatterPlanes planes = scatterPlanesFor(flags, bins, firstPlane, planeSize());
    // Hoisted: this is the innermost write in the whole engine.
    const std::size_t pitch = rowPitch();

    for (int i = rowBegin; i < rowEnd; ++i) {
        const int py = sampleRowOf(grid, region, i);
        const uint8_t* row = frame.rawPixelAt(region.x, py);
        for (int px = 0; px < region.width; px += grid.columnStride) {
            // The level a code rounds to. An 8-bit code IS its level; a deeper
            // one is rounded to the same 256, which is all this scope's bins
            // can express - see the note beside the histogram's splat.
            const Sample sample = Pixels::read(row + static_cast<std::size_t>(px) * 4);
            const int b = levelIn<Pixels, WholeLevelBits>(sample.b);
            const int g = levelIn<Pixels, WholeLevelBits>(sample.g);
            const int r = levelIn<Pixels, WholeLevelBits>(sample.r);
            // Samples splat fractionally across the two columns they
            // straddle, in sixteenths. Integer bucketing made columns
            // aggregate alternately two and three image columns at
            // typical region widths - a density comb that rendered as
            // fine vertical striping on large panes.
            const auto position = static_cast<std::size_t>(static_cast<int64_t>(px) * m_columns * 16 / region.width);
            const std::size_t column = position >> 4;
            const uint32_t rightWeight = position & 15u;
            const uint32_t leftWeight = 16u - rightWeight;
            const std::size_t next = column + 1 < static_cast<std::size_t>(m_columns) ? column + 1 : column;
            const auto splat = [&](uint32_t* plane, int level, uint32_t value) {
                uint32_t* line = plane + static_cast<std::size_t>(WaveformLevels - 1 - level) * pitch;
                line[column] += leftWeight * value;
                line[next] += rightWeight * value;
            };
            if (flags.rgb) {
                splat(planes.red, r, 1);
                splat(planes.green, g, 1);
                splat(planes.blue, b, 1);
            }
            // Luma is taken on the frame's own code scale and converted after,
            // so that an 8-bit frame reaches the level its weights have always
            // rounded it to: the truncation in luma709 is not distributive
            // over the conversion, and taking it the other way round would
            // move every eight-bit luma trace by up to a level.
            if (flags.coloredLuma) {
                // The luma plane carries the density; the channel
                // planes carry value-weighted mass at the same rows,
                // so each cell remembers the average color of the
                // pixels that landed on it.
                const int level = levelIn<Pixels, WholeLevelBits>(luma709(sample.r, sample.g, sample.b));
                splat(planes.red, level, static_cast<uint32_t>(r));
                splat(planes.green, level, static_cast<uint32_t>(g));
                splat(planes.blue, level, static_cast<uint32_t>(b));
                splat(planes.luma, level, 1);
            } else if (flags.luma) {
                splat(planes.luma, levelIn<Pixels, WholeLevelBits>(luma709(sample.r, sample.g, sample.b)), 1);
            }
        }
    }
}

void WaveformBins::scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                               uint32_t* bins, int firstPlane, WaveformMode mode) const
{
    // Dispatched once per chunk, never per pixel.
    if (frame.format == PixelFormat::Argb2101010) {
        scatterRowsAs<Argb2101010Pixels>(frame, region, grid, rowBegin, rowEnd, bins, firstPlane, mode);
    } else {
        scatterRowsAs<Bgra8Pixels>(frame, region, grid, rowBegin, rowEnd, bins, firstPlane, mode);
    }
}

void WaveformBins::scatter(const FrameView& frame, IntRect region, const SampleGrid& grid, WaveformMode mode,
                           int columns)
{
    ensureBuffers(columns);

    const WaveformPlaneSpan span = waveformPlaneSpan(waveformModeFlags(mode));
    const std::size_t spanOffset = static_cast<std::size_t>(span.first) * planeSize();
    const std::size_t spanCount = static_cast<std::size_t>(span.count) * planeSize();

    const int chunks = parallelChunkCount(grid.rows, AccumulateRowsPerChunk);
    if (chunks <= 1) {
        std::fill_n(m_bins.data() + spanOffset, spanCount, uint32_t{0});
        scatterRows(frame, region, grid, 0, grid.rows, m_bins.data() + spanOffset, span.first, mode);

        return;
    }

    // Each chunk owns a private plane set it clears and scatters into;
    // integer addition then merges them back to a bit-exact total.
    uint32_t* threadBins = m_scratch.borrow(spanCount * static_cast<std::size_t>(chunks));
    runParallelChunks(chunks, grid.rows, [&](int chunk, int begin, int end) {
        uint32_t* bins = threadBins + static_cast<std::size_t>(chunk) * spanCount;
        std::fill_n(bins, spanCount, uint32_t{0});
        scatterRows(frame, region, grid, begin, end, bins, span.first, mode);
    });
    mergeBins(threadBins, spanCount, chunks, m_bins.data() + spanOffset);
}

}  // namespace sidescopes
