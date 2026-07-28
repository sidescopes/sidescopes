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

/// Whether the pass @p held describes answers a request for @p wanted.
///
/// Every field but the plane range must be equal, and the range must CONTAIN
/// the one asked for. Wider is an answer - the pass wrote those planes from
/// these pixels - while narrower never is, because the planes it skipped hold
/// an older frame. Compared by zeroing the one field that is allowed to differ,
/// so a field added to the key is compared without this having to learn it.
bool answersRequest(const WaveformScatterKey& held, const WaveformScatterKey& wanted)
{
    WaveformScatterKey heldRest = held;
    WaveformScatterKey wantedRest = wanted;
    heldRest.span = WaveformPlaneSpan{0, 0};
    wantedRest.span = WaveformPlaneSpan{0, 0};

    return heldRest == wantedRest && held.span.count > 0 && held.span.first <= wanted.span.first &&
           held.span.first + held.span.count >= wanted.span.first + wanted.span.count;
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

WaveformPlaneSpan waveformSpanUnion(const WaveformPlaneSpan& a, const WaveformPlaneSpan& b)
{
    if (a.count <= 0) {
        return b;
    }
    if (b.count <= 0) {
        return a;
    }
    const int first = std::min(a.first, b.first);
    const int last = std::max(a.first + a.count, b.first + b.count);

    return WaveformPlaneSpan{first, last - first};
}

WaveformMode waveformModeForSpan(const WaveformPlaneSpan& span, WaveformMode requested)
{
    if (waveformModeFlags(requested).coloredLuma) {
        return requested;
    }
    // The plane range decides the mode, and only one range needs a mode no
    // scope asks for: all four planes, which is an RGB scatter and a luma one
    // over the same pixels.
    if (span.first + span.count > 3 && span.first == 0) {
        return WaveformMode::RgbAndLuma;
    }

    return requested;
}

WaveformScatterKey WaveformBins::subjectOf(const WaveformScatterKey& key)
{
    WaveformScatterKey subject = key;
    subject.span = WaveformPlaneSpan{0, 0};
    subject.coloredLuma = false;

    return subject;
}

WaveformPlaneSpan WaveformBins::spanToScatter(const WaveformPlaneSpan& wanted) const
{
    // A colored-luma request needs no special case here: it already asks for
    // all four planes, so nothing recorded can widen it.
    return waveformSpanUnion(wanted, m_wantedBefore);
}

void WaveformBins::ensureBuffers(int columns)
{
    if (!m_bins.empty() && m_columns == columns) {
        return;
    }
    m_columns = columns;
    m_bins.assign(planeSize() * 4, 0);
    // Zeroed bins hold no scatter at all, and the key must say so. A column
    // change already differs from every key that came before it, so this is
    // belt and braces - but a key outliving the bins it describes is exactly
    // the failure that would hand a scope somebody else's frame.
    m_key = WaveformScatterKey{};
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

void WaveformBins::scatterInto(const FrameView& frame, IntRect region, const SampleGrid& grid, WaveformMode mode,
                               const WaveformPlaneSpan& span)
{
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

void WaveformBins::scatter(const FrameView& frame, IntRect region, const SampleGrid& grid, WaveformMode mode,
                           int columns)
{
    ensureBuffers(columns);

    const WaveformModeFlags flags = waveformModeFlags(mode);
    const WaveformPlaneSpan span = waveformPlaneSpan(flags);
    const WaveformScatterKey key{frame.pixels, frame.strideBytes, frame.sequence, frame.format,     region,
                                 grid,         columns,           span,           flags.coloredLuma};

    // Recorded before the answer below, not after it: a scope whose request was
    // already met still wants those planes, and forgetting that would narrow
    // the next frame's pass straight back onto two.
    const WaveformScatterKey subject = subjectOf(key);
    if (m_subject != subject) {
        m_subject = subject;
        m_wantedBefore = m_wanted;
        m_wanted = WaveformPlaneSpan{0, 0};
    }
    if (!flags.coloredLuma) {
        m_wanted = waveformSpanUnion(m_wanted, span);
    }

    if (answersRequest(m_key, key)) {
        return;
    }

    const WaveformPlaneSpan scattered = spanToScatter(span);
    // Cleared before the pass rather than written after it: a scatter that
    // threw would otherwise leave half-filled bins under a key claiming they
    // hold this frame, and every scope after it would read that as an answer.
    m_key = WaveformScatterKey{};
    ++m_scatters;
    scatterInto(frame, region, grid, waveformModeForSpan(scattered, mode), scattered);
    // Under the planes it REALLY wrote, which is what the next scope's request
    // is judged against.
    m_key = key;
    m_key.span = scattered;
}

}  // namespace sidescopes
