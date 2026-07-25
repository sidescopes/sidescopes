#include "core/scopes/histogram.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/parallel_for.h"
#include "core/scopes/sampling.h"
#include "core/scopes/trace_response.h"

namespace sidescopes {
namespace {

// Sampled rows below this per chunk keep the accumulate single-threaded: a
// small region's scatter finishes before spawned threads would pay off.
constexpr int AccumulateRowsPerChunk = 64;

// The spline height for one channel at one column: Catmull-Rom through the
// neighboring bins. The spline may overshoot near sharp features; the plot
// stays within the panel, and stretches between empty bins stay empty.
double splineHeight(const double* plane, int center, const CatmullRomWeights<double>& weights, double panelHeight)
{
    const auto at = [&](int index) { return plane[std::clamp(index, 0, Histogram::Bins - 1)]; };
    const double p0 = at(center - 1);
    const double p1 = at(center);
    const double p2 = at(center + 1);
    const double p3 = at(center + 2);
    double height = weights.w0 * p0 + weights.w1 * p1 + weights.w2 * p2 + weights.w3 * p3;
    if (p1 <= 0.0 && p2 <= 0.0) {
        height = 0.0;
    }

    return std::clamp(height, 0.0, panelHeight);
}

// The fill's top edge fades over a few rows rather than one: the pane can
// magnify the plot, and a single-pixel edge would come back as a ladder on
// the slopes.
constexpr double Feather = 2.5;

/// Where one column's fill sits for one channel: the top edge in fractional
/// rows, and the half-open row range it touches. An empty range is a column
/// the channel does not reach.
struct ColumnEdge
{
    double top = 0.0;
    int firstRow = 0;
    int lastRow = 0;
};

/// Where every column's fill starts and stops, per channel, laid out channel
/// by channel. The filled area under a Catmull-Rom spline through the bin
/// heights, evaluated per image column: a plotted function with no kinks at
/// the bins. Resolving the edges first is what lets the fill run row by row.
std::vector<ColumnEdge> columnEdges(const std::vector<double>& heights, int width, int height, bool split)
{
    const int bandHeight = split ? height / 3 : height;
    std::vector<ColumnEdge> edges(static_cast<std::size_t>(width) * 3);
    for (int x = 0; x < width; ++x) {
        const double binPosition = std::clamp((x + 0.5) * Histogram::Bins / width - 0.5, 0.0, Histogram::Bins - 1.0);
        const int center = static_cast<int>(binPosition);
        const CatmullRomWeights<double> weights = catmullRomWeights(binPosition - center);
        for (int channel = 0; channel < 3; ++channel) {
            const double* plane = heights.data() + static_cast<std::ptrdiff_t>(channel) * Histogram::Bins;
            double columnHeight = splineHeight(plane, center, weights, static_cast<double>(height));
            if (split) {
                columnHeight /= 3.0;
            }
            if (columnHeight <= 0.0) {
                continue;
            }
            // Red on top, then green, then blue, when split.
            const int bandBottom = split ? (channel + 1) * bandHeight : height;
            const double top = bandBottom - columnHeight;
            ColumnEdge& edge = edges[static_cast<std::size_t>(channel) * width + x];
            edge.top = top;
            edge.firstRow = std::max(bandBottom - bandHeight, static_cast<int>(std::floor(top - Feather)));
            edge.lastRow = bandBottom;
        }
    }

    return edges;
}

}  // namespace

Histogram::Histogram()
    : m_bins(static_cast<std::size_t>(Bins) * 3, 0)
{
    resize(ImageWidth, Height);
}

void Histogram::resize(int width, int height)
{
    m_width = width;
    m_height = height;
    m_image.width = m_width;
    m_image.height = m_height;
    m_image.rgba.assign(static_cast<std::size_t>(m_width) * m_height * 4, 0);
}

void Histogram::configure(const HistogramSettings& settings)
{
    m_settings = settings;
    m_settings.samplingStride = std::clamp(m_settings.samplingStride, 1, 8);
    m_settings.imageWidth = std::clamp(m_settings.imageWidth, Bins, 4096);
    m_settings.imageHeight = std::clamp(m_settings.imageHeight, 192, 1536);
    if (m_settings.imageWidth != m_width || m_settings.imageHeight != m_height) {
        resize(m_settings.imageWidth, m_settings.imageHeight);
    }
}

void Histogram::scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                            uint32_t* bins)
{
    uint32_t* redBins = bins;
    uint32_t* greenBins = bins + Bins;
    uint32_t* blueBins = bins + static_cast<std::ptrdiff_t>(2) * Bins;

    for (int i = rowBegin; i < rowEnd; ++i) {
        const int py = sampleRowOf(grid, region, i);
        const uint8_t* row = frame.pixelAt(region.x, py);
        for (int px = 0; px < region.width; px += grid.columnStride) {
            const uint8_t* pixel = row + static_cast<std::size_t>(px) * 4;
            ++blueBins[pixel[0]];
            ++greenBins[pixel[1]];
            ++redBins[pixel[2]];
        }
    }
}

void Histogram::accumulate(const FrameView& frame, IntRect region)
{
    region = region.clampedTo(frame.width, frame.height);
    const SampleGrid grid = sampleGridFor(m_settings.samplingStride, region, SampleBudget);
    const int rowCount = grid.rows;

    const int chunks = parallelChunkCount(rowCount, AccumulateRowsPerChunk);
    if (chunks <= 1) {
        std::fill(m_bins.begin(), m_bins.end(), 0u);
        scatterRows(frame, region, grid, 0, rowCount, m_bins.data());
    } else {
        // Each chunk owns a private bin set it clears and scatters into;
        // integer addition then merges them back to a bit-exact total.
        const std::size_t binCount = m_bins.size();
        m_threadBins.resize(binCount * static_cast<std::size_t>(chunks));
        runParallelChunks(chunks, rowCount, [&](int chunk, int begin, int end) {
            uint32_t* bins = m_threadBins.data() + static_cast<std::size_t>(chunk) * binCount;
            std::fill_n(bins, binCount, uint32_t{0});
            scatterRows(frame, region, grid, begin, end, bins);
        });
        mergeBins(m_threadBins.data(), binCount, chunks, m_bins.data());
    }

    mapBinsToImage();
}

void Histogram::mapBinsToImage()
{
    const std::vector<double> heights = computeHeights();
    exportOutline(heights);
    renderFill(heights);
    ++m_image.sequence;
}

std::vector<double> Histogram::computeHeights() const
{
    // A gaussian-ish 1-4-6-4-1 kernel against bin-to-bin comb: real
    // photographs produce ragged neighboring counts that read as noise at
    // this size.
    std::vector<double> smoothed(static_cast<std::size_t>(Bins) * 3, 0.0);
    for (int channel = 0; channel < 3; ++channel) {
        const uint32_t* plane = m_bins.data() + static_cast<std::ptrdiff_t>(channel) * Bins;
        double* out = smoothed.data() + static_cast<std::ptrdiff_t>(channel) * Bins;
        for (int value = 0; value < Bins; ++value) {
            const auto at = [&](int index) { return plane[std::clamp(index, 0, Bins - 1)]; };
            out[value] =
                (at(value - 2) + 4.0 * at(value - 1) + 6.0 * at(value) + 4.0 * at(value + 1) + at(value + 2)) / 16.0;
        }
    }

    double densest = 0.0;
    for (const double count : smoothed) {
        densest = std::max(densest, count);
    }

    // Square-root heights, normalized to the tallest bin: the photo
    // editors' compromise. Linear demands per-image zoom the moment one
    // tone dominates, and a log curve melts every spike into a plateau;
    // the square root keeps a population spike a spike while sparse
    // tonal tails stay visible. Parameter-free by design.
    const auto barHeight = [&](double count) -> double {
        if (count <= 0.0 || densest <= 0.0) {
            return 0.0;
        }
        return std::max(1.0, std::sqrt(count / densest) * m_height);
    };

    std::vector<double> heights(static_cast<std::size_t>(Bins) * 3);
    for (int channel = 0; channel < 3; ++channel) {
        for (int value = 0; value < Bins; ++value) {
            heights[static_cast<std::size_t>(channel) * Bins + value] =
                barHeight(smoothed[static_cast<std::size_t>(channel) * Bins + value]);
        }
    }
    return heights;
}

void Histogram::exportOutline(const std::vector<double>& heights)
{
    // The curve, exported for the interface to stroke at display
    // resolution; full scale in both styles, the drawing side handles
    // the bands.
    m_outline.assign(static_cast<std::size_t>(Bins) * 3, 0.0f);
    for (int channel = 0; channel < 3; ++channel) {
        for (int value = 0; value < Bins; ++value) {
            m_outline[static_cast<std::size_t>(channel) * Bins + value] =
                static_cast<float>(heights[static_cast<std::size_t>(channel) * Bins + value] / m_height);
        }
    }
}

void Histogram::renderFill(const std::vector<double>& heights)
{
    // Combined: three channels overlaid over the full height. Per channel:
    // three stacked bands, each holding one channel's plot. The texture
    // carries only the dim solid fill - overlaps sum per component, so two
    // fills make a dim secondary and all three a quiet neutral gray. The
    // bright outline is NOT baked here: the pane stretches this texture
    // anisotropically, which would render any baked stroke thick on flats and
    // thin on slopes; the interface strokes the exported curve at display
    // resolution instead.
    //
    // Written a row at a time. Walking columns instead meant every store in a
    // column landed a row-stride apart - kilobytes, for adjacent writes - and
    // opaque alpha was stamped one strided byte per pixel; now the clear lays
    // it down along the buffer and the fill runs along each row.
    constexpr double FillValue = 118.0;
    const bool split = m_settings.style == HistogramStyle::PerChannel;
    const std::vector<ColumnEdge> edges = columnEdges(heights, m_width, m_height, split);
    for (std::size_t i = 0; i + 3 < m_image.rgba.size(); i += 4) {
        m_image.rgba[i] = 0;
        m_image.rgba[i + 1] = 0;
        m_image.rgba[i + 2] = 0;
        m_image.rgba[i + 3] = 255;
    }
    for (int row = 0; row < m_height; ++row) {
        uint8_t* line = m_image.rgba.data() + static_cast<std::size_t>(row) * m_width * 4;
        for (int channel = 0; channel < 3; ++channel) {
            const ColumnEdge* channelEdges = edges.data() + static_cast<std::size_t>(channel) * m_width;
            for (int x = 0; x < m_width; ++x) {
                const ColumnEdge& edge = channelEdges[x];
                if (row < edge.firstRow || row >= edge.lastRow) {
                    continue;
                }
                const double coverage = std::clamp((row + 1.0 - edge.top) / Feather, 0.0, 1.0);
                if (coverage <= 0.0) {
                    continue;
                }
                uint8_t* pixel = line + static_cast<std::size_t>(x) * 4 + channel;
                *pixel = static_cast<uint8_t>(std::max<double>(*pixel, FillValue * coverage));
            }
        }
    }
}

}  // namespace sidescopes
