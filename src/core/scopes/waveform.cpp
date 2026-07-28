#include "core/scopes/waveform.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/parallel_for.h"
#include "core/scopes/sampling.h"
#include "core/scopes/trace_response.h"

namespace sidescopes {
namespace {

// Below this many image rows per chunk the compose stays single-threaded: the
// work finishes before spawned threads would pay off.
constexpr int ComposeRowsPerChunk = 16;

// Rec.709 luma weights in float, for the sub-level projection: the level marker
// wants a fractional position, not the accumulator's truncated code.
inline float luma709(float r, float g, float b)
{
    return (54.0f * r + 183.0f * g + 19.0f * b) / 256.0f;
}

// A waveform column is populated by one sample per sampled row, so densities
// are normalized per sampled row: column brightness is then invariant to the
// sampling stride and to the region size.
constexpr double ReferenceRowCount = 1'000.0;

// In the combined mode the luma trace rides over the RGB traces dimmed to
// this fraction, so it reads as a distinct overlay rather than a fourth channel.
constexpr float RgbLumaDim = 0.7f;

// Sum each level's population across a plane's columns.
//
// `rowPitch` is the distance between levels and `columns` is how many bins a
// level really holds. They are NOT the same number and must never be merged
// back into one parameter - see Waveform::rowPitch.
void sumLevelDensities(const uint32_t* in, std::size_t rowPitch, int columns, uint64_t* global)
{
    for (int row = 0; row < WaveformLevels; ++row) {
        const uint32_t* line = in + static_cast<std::size_t>(row) * rowPitch;
        for (int column = 0; column < columns; ++column) {
            global[row] += line[column];
        }
    }
}

// The levels bracketing the populated range, inclusive.
struct PopulatedRange
{
    int lowest;
    int highest;
};

// The populated range: spikes at its edges are real clipping
// lines - crushed blacks, blown whites - and stay protected.
PopulatedRange populatedRange(const uint64_t* global)
{
    int lowest = WaveformLevels;
    int highest = -1;
    for (int row = 0; row < WaveformLevels; ++row) {
        if (global[row] == 0) {
            continue;
        }
        if (lowest == WaveformLevels) {
            lowest = row;
        }
        highest = row;
    }
    return {lowest, highest};
}

// The neighborhood MEDIAN over +-6 levels, the level itself excluded.
double neighborhoodMedian(const uint64_t* global, int row)
{
    uint64_t neighborhood[12];
    int counted = 0;
    for (int neighbor = row - 6; neighbor <= row + 6; ++neighbor) {
        if (neighbor == row || neighbor < 0 || neighbor >= WaveformLevels) {
            continue;
        }
        neighborhood[counted++] = global[neighbor];
    }
    // Insertion sort with explicit bounds: the array is tiny, and
    // std::sort here trips GCC's array-bounds analysis.
    for (int i = 1; i < counted; ++i) {
        const uint64_t value = neighborhood[i];
        int j = i - 1;
        while (j >= 0 && neighborhood[j] > value) {
            neighborhood[j + 1] = neighborhood[j];
            --j;
        }
        neighborhood[j + 1] = value;
    }
    const int middle = counted / 2;
    return counted > 0 ? static_cast<double>(neighborhood[middle]) : 0.0;
}

// Flat-field weights in 1/256ths: the neighborhood MEDIAN over
// +-6 levels against the level's own population. The median
// matters: a genuinely dominant flat tone is a huge real spike,
// and a mean would inflate its neighbors' expected density and
// over-lift them - manufacturing the very banding this removes.
void computeFlattenWeights(const uint64_t* global, int lowest, int highest, uint32_t* flatten, double* expectedOf)
{
    for (int row = 0; row < WaveformLevels; ++row) {
        const double expected = neighborhoodMedian(global, row);
        expectedOf[row] = expected;

        // A pipeline pileup steals its mass from nearby codes, so it
        // always travels with starved neighbors inside the populated
        // range; a real flat tone starves nothing, and a real
        // clipping line sits at the range's edge. Only the pileups
        // may be attenuated without limit.
        bool starvedNearby = false;
        if (expected > 0.0) {
            for (int neighbor = row - 4; neighbor <= row + 4; ++neighbor) {
                if (neighbor == row || neighbor <= lowest + 1 || neighbor >= highest - 1) {
                    continue;
                }
                if (static_cast<double>(global[neighbor]) < expected * 0.1) {
                    starvedNearby = true;
                }
            }
        }
        const bool interior = row > lowest + 2 && row < highest - 2;
        const double attenuationFloor = (starvedNearby && interior) ? 1.0 / 64.0 : 1.0 / 3.0;

        double weight = 1.0;
        if (global[row] > 0 && expected > 0.0) {
            weight = std::clamp(expected / static_cast<double>(global[row]), attenuationFloor, 3.0);
        }
        flatten[row] = static_cast<uint32_t>(weight * 256.0);
    }
}

// Codes the pipeline never emits cannot be lifted by weighting -
// their counts are zero. They are reconstructed instead: a
// starved interior code takes the distance-weighted mix of its
// nearest healthy neighbors, per column, so the trace reads as
// the continuous signal the display quantized away. Only short
// gaps qualify; wider ones are honest emptiness relative to
// their neighborhood and stay dark.
void computeMixTargets(const uint64_t* global, const double* expectedOf, int lowest, int highest, int* mixAbove,
                       int* mixBelow)
{
    const auto healthy = [&](int row) {
        return expectedOf[row] <= 0.0 || static_cast<double>(global[row]) >= expectedOf[row] * 0.25;
    };
    for (int row = 0; row < WaveformLevels; ++row) {
        mixAbove[row] = row;
        mixBelow[row] = row;
        const bool interior = row > lowest + 2 && row < highest - 2;
        if (!interior || healthy(row)) {
            continue;
        }
        int above = -1;
        for (int neighbor = row - 1; neighbor >= row - 3 && neighbor >= 0; --neighbor) {
            if (healthy(neighbor)) {
                above = neighbor;
                break;
            }
        }
        int below = -1;
        for (int neighbor = row + 1; neighbor <= row + 3 && neighbor < WaveformLevels; ++neighbor) {
            if (healthy(neighbor)) {
                below = neighbor;
                break;
            }
        }
        if (above >= 0 && below >= 0) {
            mixAbove[row] = above;
            mixBelow[row] = below;
        }
    }
}

// Apply the flat-field weights, reconstructing dead interior codes
// from their healthy neighbors, into the per-plane corrected buffer.
void applyCorrection(const uint32_t* in, std::size_t rowPitch, int columns, const uint32_t* flatten,
                     const int* mixAbove, const int* mixBelow, uint32_t* corrected)
{
    for (int row = 0; row < WaveformLevels; ++row) {
        uint32_t* line = corrected + static_cast<std::size_t>(row) * rowPitch;
        const auto weighted = [&](int level, int column) -> uint32_t {
            const uint64_t count = in[static_cast<std::size_t>(level) * rowPitch + column];
            return static_cast<uint32_t>(count * flatten[level] >> 8);
        };
        if (mixAbove[row] == row) {
            for (int column = 0; column < columns; ++column) {
                line[column] = weighted(row, column);
            }
        } else {
            const int above = mixAbove[row];
            const int below = mixBelow[row];
            const uint32_t gap = static_cast<uint32_t>(below - above);
            const uint32_t belowShare = static_cast<uint32_t>(row - above);
            const uint32_t aboveShare = static_cast<uint32_t>(below - row);
            for (int column = 0; column < columns; ++column) {
                line[column] = (weighted(above, column) * aboveShare + weighted(below, column) * belowShare) / gap;
            }
        }
    }
}

// Smooth a corrected plane into the output plane: a vertical 1-4-1
// then a horizontal 1-2-1.
void smoothPlane(const uint32_t* corrected, std::size_t rowPitch, int columns, uint32_t* out)
{
    // A plane with no columns holds nothing to smooth. Stated rather than left
    // to the loop bounds because the analyzer cannot otherwise tell that the
    // row pointers below are formed from real storage.
    if (corrected == nullptr || out == nullptr || columns <= 0) {
        return;
    }

    // Vertical 1-4-1: light, so a sharp level stays crisp while
    // single-bin grain still fills in. The banding work lives in the
    // flat-field and the dead-code reconstruction above - a wider
    // kernel here only blurred what they had already repaired, and
    // big panes magnified that blur.
    //
    // Walked one level at a time with the three taps held as row pointers.
    // Column-major traversal reread the whole plane once per column - at a
    // wide pane that is megabytes restreamed thousands of times - while the
    // arithmetic per bin is identical either way.
    for (int row = 0; row < WaveformLevels; ++row) {
        const uint32_t* self = corrected + static_cast<std::size_t>(row) * rowPitch;
        const uint32_t* above = row > 0 ? self - rowPitch : nullptr;
        const uint32_t* below = row + 1 < WaveformLevels ? self + rowPitch : nullptr;
        uint32_t* line = out + static_cast<std::size_t>(row) * rowPitch;
        for (int column = 0; column < columns; ++column) {
            const uint32_t lower = above != nullptr ? above[column] : 0u;
            const uint32_t upper = below != nullptr ? below[column] : 0u;
            line[column] = (lower + 4 * self[column] + upper + 3) / 6;
        }
    }
    // Horizontal 1-2-1 within each row, in place.
    for (int row = 0; row < WaveformLevels; ++row) {
        uint32_t* line = out + static_cast<std::size_t>(row) * rowPitch;
        uint32_t previous = 0;
        for (int column = 0; column < columns; ++column) {
            const uint32_t current = line[column];
            const uint32_t next = column + 1 < columns ? line[column + 1] : 0;
            line[column] = (previous + 2 * current + next + 2) / 4;
            previous = current;
        }
    }
}

// A column is one flat tone when its densest bin holds at least this share of
// the column's own mass. Held as a fraction so the test is integer arithmetic:
// the goldens are exact and shared across three platforms, and a float
// comparison here would decide the whole image's brightness.
//
// The vertical 1-4-1 fixes what the share means. A column whose samples all
// land on one level keeps 4/6 of its mass there; two adjacent levels give
// 5/12, and from three levels up it is simply one over the level count. So
// three tenths reads as "fewer than about three and a third levels" - a flat
// tone, plus however far the display's dithering spreads it. Measured over
// four photographs at five modes, four column counts and three region sizes,
// the highest a photographic column reached was a quarter: 240 of 240 of those
// renderings are bit-identical at this share, and the first of them moves at
// 0.22.
constexpr uint64_t FlatToneNumerator = 3;
constexpr uint64_t FlatToneDenominator = 10;

// Each column's densest bin, the level it sits at, and the column's total
// mass, over the bins the plane really holds: the space between planes is
// padding, never written, and reading it would let a stale word from an
// earlier size decide the normalization ceiling.
void measureColumns(const uint32_t* bins, std::size_t rowPitch, int columns, ColumnDensity* densities)
{
    std::fill_n(densities, columns, ColumnDensity{});
    for (int level = 0; level < WaveformLevels; ++level) {
        const uint32_t* line = bins + static_cast<std::size_t>(level) * rowPitch;
        for (int column = 0; column < columns; ++column) {
            ColumnDensity& measured = densities[static_cast<std::size_t>(column)];
            measured.mass += line[column];
            if (line[column] > measured.peak) {
                measured.peak = line[column];
                measured.level = level;
            }
        }
    }
}

// The levels some column of this plane holds as one flat tone.
//
// Only the level each such column peaks on, with no allowance for the vertical
// 1-4-1 spreading the pile onto its neighbours: the columns that carry a
// diluted copy of the same pile are weighted level by level exactly as this one
// is, so they peak wherever it peaks. Widening by one either side was measured
// over a flat tone at all 256 levels, three strip widths and two modes - it
// moved six of those 1536 traces, none of them by a measurable change in
// brightness.
void markFlatToneLevels(const ColumnDensity* densities, int columns, bool* flatTone)
{
    std::fill_n(flatTone, WaveformLevels, false);
    for (int column = 0; column < columns; ++column) {
        const ColumnDensity& measured = densities[static_cast<std::size_t>(column)];
        if (measured.mass > 0 &&
            static_cast<uint64_t>(measured.peak) * FlatToneDenominator > FlatToneNumerator * measured.mass) {
            flatTone[measured.level] = true;
        }
    }
}

// The ceiling a plane offers: the densest bin among its columns that peak on a
// level no flat tone holds, and - as the fallback - the densest of all of them.
struct TraceCeiling
{
    uint32_t distributed = 0;
    uint32_t overall = 0;
};

void gatherCeiling(const ColumnDensity* densities, int columns, const bool* flatTone, TraceCeiling& ceiling)
{
    for (int column = 0; column < columns; ++column) {
        const ColumnDensity& measured = densities[static_cast<std::size_t>(column)];
        ceiling.overall = std::max(ceiling.overall, measured.peak);
        // A flat tone does not end at the edge of the strip that carries it:
        // the fractional splat and the horizontal 1-2-1 taper its pile over the
        // columns beside it, several times the surrounding content but too
        // diluted to read as one tone. What gives those columns away is that
        // they still peak on the tone's own level.
        if (!flatTone[measured.level]) {
            ceiling.distributed = std::max(ceiling.distributed, measured.peak);
        }
    }
}

// The bin the log normalization maps to full brightness, across the planes the
// active mode draws.
//
// Not simply the densest bin. A column that is one flat tone - the editor's
// chrome beside the photograph, a letterboxed bar - puts every sample it has
// into a single bin, several times over what the densest photographic column
// reaches, and the ceiling is close to linear in that at the calibrated gain.
// Sliding a region five pixels off the picture cost the rest of the trace a
// quarter of its brightness. Such a column is passed over instead, and its
// bins clip. When every column is one flat tone there is no distribution to
// measure and the plain maximum stands, which is what keeps a frame of colour
// bars or a ramp rendering exactly as before.
uint32_t peakDensity(const std::vector<uint32_t>& traces, std::size_t planeSize, std::size_t rowPitch, int columns,
                     bool wantsRgb, bool wantsLuma, ColumnDensity* densities)
{
    TraceCeiling ceiling;
    bool flatTone[WaveformLevels];
    const auto scan = [&](int plane) {
        measureColumns(traces.data() + (static_cast<std::size_t>(plane) * planeSize), rowPitch, columns, densities);
        markFlatToneLevels(densities, columns, flatTone);
        gatherCeiling(densities, columns, flatTone, ceiling);
    };
    if (wantsRgb) {
        for (int plane = 0; plane < 3; ++plane) {
            scan(plane);
        }
    }
    if (wantsLuma) {
        scan(3);
    }

    return ceiling.distributed > 0 ? ceiling.distributed : ceiling.overall;
}

// Map a bin count to display brightness through the log-and-gamma
// response shared with the vectorscope.
float waveformBrightness(float count, double gain, double intensityScale)
{
    if (count <= 0.0f) {
        return 0.0f;
    }
    // The gamma lifts the mid-density body of the trace, exactly as
    // on the vectorscope: normalizing to the densest bin pushes
    // everything else down, and a linear ramp reads dim at any gain.
    const double normalized = std::log1p(static_cast<double>(count) * gain) * intensityScale / 255.0;
    return static_cast<float>(255.0 * applyMidDensityGamma(normalized));
}

// The four smoothed planes the composer reads, in draw order.
struct WaveformPlanes
{
    const uint32_t* red;
    const uint32_t* green;
    const uint32_t* blue;
    const uint32_t* luma;
};

// Resolve one output pixel from the planes at a column, applying the
// active mode's color rules. sample() reads a plane at the output row's
// level tap and is invoked only for the planes the mode draws.
template <typename SampleFn>
void emitWaveformPixel(uint8_t* out, const SampleFn& sample, int column, const WaveformPlanes& planes,
                       const WaveformModeFlags& flags, double gain, double intensityScale)
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (flags.rgb) {
        r = waveformBrightness(sample(planes.red, column), gain, intensityScale);
        g = waveformBrightness(sample(planes.green, column), gain, intensityScale);
        b = waveformBrightness(sample(planes.blue, column), gain, intensityScale);
    }
    if (flags.coloredLuma) {
        // Density decides how bright the trace is; the
        // value-weighted planes only decide its color, so a
        // dense shadow region draws as clearly as a dense
        // highlight, each in its own tint.
        const float density = waveformBrightness(sample(planes.luma, column), gain, intensityScale);
        const float massR = sample(planes.red, column);
        const float massG = sample(planes.green, column);
        const float massB = sample(planes.blue, column);
        const float strongest = std::max({massR, massG, massB});
        if (strongest > 0.0f) {
            r = density * (massR / strongest);
            g = density * (massG / strongest);
            b = density * (massB / strongest);
        } else {
            r = g = b = density;
        }
    } else if (flags.luma) {
        // In the combined mode luma rides on top as a dimmer
        // white trace.
        const float luma =
            waveformBrightness(sample(planes.luma, column), gain, intensityScale) * (flags.rgb ? RgbLumaDim : 1.0f);
        r += luma;
        g += luma;
        b += luma;
    }
    out[0] = static_cast<uint8_t>(std::min(255.0f, r));
    out[1] = static_cast<uint8_t>(std::min(255.0f, g));
    out[2] = static_cast<uint8_t>(std::min(255.0f, b));
    out[3] = 255;
}

// The level-axis sampling tap for one output row: a base level and the
// four Catmull-Rom weights (base = row, unit weight at native height).
struct LevelSample
{
    int base;
    float weight0;
    float weight1;
    float weight2;
    float weight3;
};

LevelSample levelSampleWeights(int y, int imageHeight, bool nativeHeight)
{
    LevelSample tap{y, 0.0f, 1.0f, 0.0f, 0.0f};
    if (!nativeHeight) {
        const float position = (static_cast<float>(y) + 0.5f) * WaveformLevels / static_cast<float>(imageHeight) - 0.5f;
        const float floored = std::floor(position);
        tap.base = static_cast<int>(floored);
        const CatmullRomWeights<float> weights = catmullRomWeights(position - floored);
        tap.weight0 = weights.w0;
        tap.weight1 = weights.w1;
        tap.weight2 = weights.w2;
        tap.weight3 = weights.w3;
    }
    return tap;
}

}  // namespace

Waveform::Waveform() = default;

void Waveform::ensureBuffers()
{
    if (!m_image.rgba.empty() && m_columns == m_settings.columns && m_imageHeight == m_settings.imageHeight) {
        return;
    }
    resize(m_settings.columns, m_settings.imageHeight);
}

void Waveform::configure(const WaveformSettings& settings)
{
    m_settings = settings;
    m_settings.samplingStride = std::clamp(m_settings.samplingStride, 1, 8);
    m_settings.columns = std::clamp(m_settings.columns, 256, MaximumWaveformColumns);
    m_settings.imageHeight = std::clamp(m_settings.imageHeight, WaveformLevels, MaximumWaveformHeight);
    m_settings.sampleThinning = std::clamp(m_settings.sampleThinning, 1, 8);
}

void Waveform::resize(int columns, int imageHeight)
{
    m_columns = columns;
    m_imageHeight = imageHeight;
    m_columnDensities.assign(static_cast<std::size_t>(m_columns), ColumnDensity{});
    m_image.width = m_columns;
    m_image.height = m_imageHeight;
    m_image.rgba.assign(static_cast<std::size_t>(m_columns) * m_imageHeight * 4, 0);
}

void Waveform::accumulate(const FrameView& frame, IntRect region)
{
    ensureBuffers();
    region = region.clampedTo(frame.width, frame.height);
    const int perBin = std::max(WaveformMinSamplesPerBin / m_settings.sampleThinning, 1);
    const SampleGrid grid = sampleGridFor(m_settings.samplingStride, region,
                                          budgetForBins(static_cast<long long>(m_columns) * Levels, perBin));
    bins().scatter(frame, region, grid, m_settings.mode, m_columns);

    // A column receives one sample per sampled row, so the sampled-row count
    // that normalizes column brightness is a plain quotient - identical
    // however the rows split across threads.
    mapBinsToImage(static_cast<uint64_t>(grid.rows));
}

NormalizedPoint Waveform::project(const FloatColor& color)
{
    const float luma = luma709(color.r, color.g, color.b);
    return NormalizedPoint{-1.0f, (255.0f - luma) / 255.0f};
}

void Waveform::mapBinsToImage(uint64_t sampledRows)
{
    correctBinDensities();
    const std::vector<uint32_t>& traces = m_smoothed;
    const std::size_t planeSize = this->planeSize();

    const WaveformModeFlags flags = waveformModeFlags(m_settings.mode);
    const uint32_t densest =
        peakDensity(traces, planeSize, rowPitch(), m_columns, flags.rgb, flags.luma, m_columnDensities.data());

    // Each sample contributes sixteen weight units (the splat's
    // sixteenths), so the per-row normalization divides them back out and
    // the gain setting keeps its calibrated feel.
    const double perRowScale = sampledRows > 0 ? ReferenceRowCount / (static_cast<double>(sampledRows) * 16.0) : 0.0;
    const double gain = static_cast<double>(m_settings.gain) * perRowScale;
    const double logCeiling = densest > 0 ? std::log1p(static_cast<double>(densest) * gain) : 0.0;
    const double intensityScale = logCeiling > 0.0 ? 255.0 / logCeiling : 0.0;

    const uint32_t* redPlane = traces.data();
    const uint32_t* greenPlane = traces.data() + planeSize;
    const uint32_t* bluePlane = traces.data() + 2 * planeSize;
    const uint32_t* lumaPlane = traces.data() + 3 * planeSize;

    if (m_settings.mode == WaveformMode::RgbParade) {
        buildParade(redPlane, greenPlane, bluePlane);
        redPlane = m_parade.data();
        greenPlane = m_parade.data() + planeSize;
        bluePlane = m_parade.data() + 2 * planeSize;
    }

    composeImage(redPlane, greenPlane, bluePlane, lumaPlane, gain, intensityScale);
    ++m_image.sequence;
}

void Waveform::correctBinDensities()
{
    // The display pipeline uses the 256 codes unevenly: tone edits and
    // 8-bit rendering leave some values doubly populated and others
    // nearly empty, frame-wide. Rendered faithfully, that quantization
    // signature is horizontal banding across the whole trace - real in
    // the data, meaningless to a photographer. The banding is exactly
    // row-global, so it is measured globally and divided back out:
    // each level's density is compared against its smoothed
    // neighborhood, and the trace is corrected by that ratio. Spatial
    // structure is column-local and passes through untouched; genuinely
    // dominant flat tones exceed the clamp and survive, compressed
    // further by the log display.
    const std::size_t planeSize = this->planeSize();
    m_smoothed.resize(bins().size());
    m_corrected.resize(planeSize);
    // Only the mode's own planes: a plane it does not draw holds no counts,
    // and correcting one is the whole per-frame cost of a plane spent on an
    // image nothing reads.
    const WaveformPlaneSpan span = waveformPlaneSpan(waveformModeFlags(m_settings.mode));
    for (int plane = span.first; plane < span.first + span.count; ++plane) {
        const uint32_t* in = bins().data() + static_cast<std::size_t>(plane) * planeSize;
        uint32_t* out = m_smoothed.data() + static_cast<std::size_t>(plane) * planeSize;

        uint64_t global[Levels] = {};
        sumLevelDensities(in, rowPitch(), m_columns, global);
        const PopulatedRange range = populatedRange(global);

        uint32_t flatten[Levels];
        double expectedOf[Levels];
        computeFlattenWeights(global, range.lowest, range.highest, flatten, expectedOf);

        int mixAbove[Levels];
        int mixBelow[Levels];
        computeMixTargets(global, expectedOf, range.lowest, range.highest, mixAbove, mixBelow);

        applyCorrection(in, rowPitch(), m_columns, flatten, mixAbove, mixBelow, m_corrected.data());
        smoothPlane(m_corrected.data(), rowPitch(), m_columns, out);
    }
}

void Waveform::buildParade(const uint32_t* redPlane, const uint32_t* greenPlane, const uint32_t* bluePlane)
{
    // Three channels side by side: each third shows one channel's
    // full column range compressed 3:1, window-maxed so sparse
    // traces stay visible. The result feeds the same composer as
    // the overlaid modes.
    const std::size_t planeSize = this->planeSize();
    m_parade.assign(3 * planeSize, 0);
    const int third = m_columns / 3;
    // A dark gutter separates the panes so each channel reads as its
    // own plot instead of three traces colliding at hard seams.
    const int gutter = std::max(2, m_columns / 256);
    const uint32_t* planes[3] = {redPlane, greenPlane, bluePlane};
    for (int channel = 0; channel < 3; ++channel) {
        uint32_t* outPlane = m_parade.data() + static_cast<std::size_t>(channel) * planeSize;
        const int first = channel * third + (channel > 0 ? gutter : 0);
        const int last = (channel == 2 ? m_columns : (channel + 1) * third) - (channel < 2 ? gutter : 0);
        const int span = last - first;
        for (int row = 0; row < Levels; ++row) {
            const uint32_t* sourceRow = planes[channel] + static_cast<std::size_t>(row) * rowPitch();
            uint32_t* outRow = outPlane + static_cast<std::size_t>(row) * rowPitch();
            for (int column = first; column < last; ++column) {
                const int local = column - first;
                const int begin = local * m_columns / span;
                const int end = std::min((local + 1) * m_columns / span, m_columns);
                uint32_t densestInWindow = 0;
                for (int source = begin; source < end; ++source) {
                    densestInWindow = std::max(densestInWindow, sourceRow[source]);
                }
                outRow[column] = densestInWindow;
            }
        }
    }
}

void Waveform::composeImage(const uint32_t* redPlane, const uint32_t* greenPlane, const uint32_t* bluePlane,
                            const uint32_t* lumaPlane, double gain, double intensityScale)
{
    const WaveformModeFlags flags = waveformModeFlags(m_settings.mode);
    const WaveformPlanes planes{redPlane, greenPlane, bluePlane, lumaPlane};

    // The composer. At native height rows map one-to-one onto levels; a
    // taller image samples the level axis through a Catmull-Rom spline -
    // the histogram's technique - so a magnified trace draws as a curve
    // instead of stretched texels. Output rows are independent, so a band of
    // them runs per thread: about half the waveform's cost is this pass.
    const bool nativeHeight = m_imageHeight == Levels;
    const auto composeRows = [&](int yBegin, int yEnd) {
        uint8_t* out = m_image.rgba.data() + static_cast<std::size_t>(yBegin) * m_columns * 4;
        for (int y = yBegin; y < yEnd; ++y) {
            const LevelSample tap = levelSampleWeights(y, m_imageHeight, nativeHeight);
            const auto sample = [&](const uint32_t* plane, int column) -> float {
                const auto rowAt = [&](int level) -> float {
                    if (level < 0 || level >= Levels) {
                        return 0.0f;
                    }
                    return static_cast<float>(plane[static_cast<std::size_t>(level) * rowPitch() + column]);
                };
                if (nativeHeight) {
                    return rowAt(tap.base);
                }
                return std::max(0.0f, tap.weight0 * rowAt(tap.base - 1) + tap.weight1 * rowAt(tap.base) +
                                          tap.weight2 * rowAt(tap.base + 1) + tap.weight3 * rowAt(tap.base + 2));
            };
            for (int column = 0; column < m_columns; ++column, out += 4) {
                emitWaveformPixel(out, sample, column, planes, flags, gain, intensityScale);
            }
        }
    };

    const int chunks = parallelChunkCount(m_imageHeight, ComposeRowsPerChunk);
    runParallelChunks(chunks, m_imageHeight, [&](int, int begin, int end) { composeRows(begin, end); });
}

}  // namespace sidescopes
