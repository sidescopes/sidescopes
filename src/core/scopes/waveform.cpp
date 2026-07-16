#include "core/scopes/waveform.h"

#include <algorithm>
#include <cmath>

#include "core/scopes/trace_response.h"

namespace sidescopes {
namespace {

// Rec.709 luma weights, fixed-point x256, applied to display-encoded values.
inline int luma709(int r, int g, int b)
{
    return (54 * r + 183 * g + 19 * b) >> 8;
}

// A waveform column is populated by one sample per sampled row, so densities
// are normalized per sampled row: column brightness is then invariant to the
// sampling stride and to the region size.
constexpr double ReferenceRowCount = 1'000.0;

// Which planes a mode draws: the RGB channels, the luma trace, and whether
// luma carries the source color. Derived once and threaded through
// accumulation, correction, and composition so the three stay in step.
struct ModeFlags
{
    bool rgb;
    bool luma;
    bool coloredLuma;
};

ModeFlags modeFlagsFor(WaveformMode mode)
{
    const bool coloredLuma = mode == WaveformMode::ColoredLuma;
    return ModeFlags{
        mode != WaveformMode::Luma && !coloredLuma,
        mode == WaveformMode::Luma || mode == WaveformMode::RgbAndLuma || coloredLuma,
        coloredLuma,
    };
}

// Sum each level's population across a plane's columns.
void sumLevelDensities(const uint32_t* in, int columns, uint64_t* global)
{
    for (int row = 0; row < WaveformLevels; ++row) {
        const uint32_t* line = in + static_cast<std::size_t>(row) * columns;
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
    for (int near = row - 6; near <= row + 6; ++near) {
        if (near == row || near < 0 || near >= WaveformLevels) {
            continue;
        }
        neighborhood[counted++] = global[near];
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
            for (int near = row - 4; near <= row + 4; ++near) {
                if (near == row || near <= lowest + 1 || near >= highest - 1) {
                    continue;
                }
                if (static_cast<double>(global[near]) < expected * 0.1) {
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
        for (int near = row - 1; near >= row - 3 && near >= 0; --near) {
            if (healthy(near)) {
                above = near;
                break;
            }
        }
        int below = -1;
        for (int near = row + 1; near <= row + 3 && near < WaveformLevels; ++near) {
            if (healthy(near)) {
                below = near;
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
void applyCorrection(const uint32_t* in, int columns, const uint32_t* flatten, const int* mixAbove, const int* mixBelow,
                     uint32_t* corrected)
{
    for (int row = 0; row < WaveformLevels; ++row) {
        uint32_t* line = corrected + static_cast<std::size_t>(row) * columns;
        const auto weighted = [&](int level, int column) -> uint32_t {
            const uint64_t count = in[static_cast<std::size_t>(level) * columns + column];
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
void smoothPlane(const uint32_t* corrected, int columns, uint32_t* out)
{
    // Vertical 1-4-1: light, so a sharp level stays crisp while
    // single-bin grain still fills in. The banding work lives in the
    // flat-field and the dead-code reconstruction above - a wider
    // kernel here only blurred what they had already repaired, and
    // big panes magnified that blur.
    for (int column = 0; column < columns; ++column) {
        for (int row = 0; row < WaveformLevels; ++row) {
            const auto at = [&](int level) -> uint32_t {
                if (level < 0 || level >= WaveformLevels) {
                    return 0;
                }
                return corrected[static_cast<std::size_t>(level) * columns + column];
            };
            out[static_cast<std::size_t>(row) * columns + column] = (at(row - 1) + 4 * at(row) + at(row + 1) + 3) / 6;
        }
    }
    // Horizontal 1-2-1 within each row, in place.
    for (int row = 0; row < WaveformLevels; ++row) {
        uint32_t* line = out + static_cast<std::size_t>(row) * columns;
        uint32_t previous = 0;
        for (int column = 0; column < columns; ++column) {
            const uint32_t current = line[column];
            const uint32_t next = column + 1 < columns ? line[column + 1] : 0;
            line[column] = (previous + 2 * current + next + 2) / 4;
            previous = current;
        }
    }
}

// The densest bin across the planes the active mode draws; it sets the
// log-normalization ceiling.
uint32_t peakDensity(const std::vector<uint32_t>& traces, std::size_t planeSize, bool wantsRgb, bool wantsLuma)
{
    uint32_t densest = 0;
    if (wantsRgb) {
        for (std::size_t i = 0; i < 3 * planeSize; ++i) {
            densest = std::max(densest, traces[i]);
        }
    }
    if (wantsLuma) {
        for (std::size_t i = 3 * planeSize; i < 4 * planeSize; ++i) {
            densest = std::max(densest, traces[i]);
        }
    }
    return densest;
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
                       const ModeFlags& flags, double gain, double intensityScale)
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
            waveformBrightness(sample(planes.luma, column), gain, intensityScale) * (flags.rgb ? 0.7f : 1.0f);
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

Waveform::Waveform()
{
    resize(DefaultWaveformColumns, WaveformLevels);
}

void Waveform::configure(const WaveformSettings& settings)
{
    m_settings = settings;
    m_settings.samplingStride = std::clamp(m_settings.samplingStride, 1, 8);
    m_settings.columns = std::clamp(m_settings.columns, 256, 2048);
    m_settings.imageHeight = std::clamp(m_settings.imageHeight, WaveformLevels, 768);
    if (m_settings.columns != m_columns || m_settings.imageHeight != m_imageHeight) {
        resize(m_settings.columns, m_settings.imageHeight);
    }
}

void Waveform::resize(int columns, int imageHeight)
{
    m_columns = columns;
    m_imageHeight = imageHeight;
    m_bins.assign(planeSize() * 4, 0);
    m_image.width = m_columns;
    m_image.height = m_imageHeight;
    m_image.rgba.assign(static_cast<std::size_t>(m_columns) * m_imageHeight * 4, 0);
}

void Waveform::accumulate(const FrameView& frame, IntRect region)
{
    region = region.clampedTo(frame.width, frame.height);
    std::fill(m_bins.begin(), m_bins.end(), 0u);

    const ModeFlags flags = modeFlagsFor(m_settings.mode);
    const std::size_t planeSize = this->planeSize();
    uint32_t* redPlane = m_bins.data();
    uint32_t* greenPlane = m_bins.data() + planeSize;
    uint32_t* bluePlane = m_bins.data() + 2 * planeSize;
    uint32_t* lumaPlane = m_bins.data() + 3 * planeSize;

    uint64_t sampledRows = 0;
    if (!region.empty()) {
        const int stride = m_settings.samplingStride;
        for (int py = region.y; py < region.y + region.height; py += stride) {
            ++sampledRows;
            const uint8_t* row = frame.pixelAt(region.x, py);
            for (int px = 0; px < region.width; px += stride) {
                const uint8_t* pixel = row + static_cast<std::size_t>(px) * 4;
                const int b = pixel[0], g = pixel[1], r = pixel[2];
                // Samples splat fractionally across the two columns they
                // straddle, in sixteenths. Integer bucketing made columns
                // aggregate alternately two and three image columns at
                // typical region widths - a density comb that rendered as
                // fine vertical striping on large panes.
                const auto position =
                    static_cast<std::size_t>(static_cast<int64_t>(px) * m_columns * 16 / region.width);
                const std::size_t column = position >> 4;
                const uint32_t rightWeight = position & 15u;
                const uint32_t leftWeight = 16u - rightWeight;
                const std::size_t next = column + 1 < static_cast<std::size_t>(m_columns) ? column + 1 : column;
                const auto splat = [&](uint32_t* plane, int value) {
                    uint32_t* line = plane + static_cast<std::size_t>(255 - value) * m_columns;
                    line[column] += leftWeight;
                    line[next] += rightWeight;
                };
                if (flags.rgb) {
                    splat(redPlane, r);
                    splat(greenPlane, g);
                    splat(bluePlane, b);
                }
                if (flags.coloredLuma) {
                    // The luma plane carries the density; the channel
                    // planes carry value-weighted mass at the same rows,
                    // so each cell remembers the average color of the
                    // pixels that landed on it.
                    const int level = luma709(r, g, b);
                    const auto tintSplat = [&](uint32_t* plane, uint32_t value) {
                        uint32_t* line = plane + static_cast<std::size_t>(255 - level) * m_columns;
                        line[column] += leftWeight * value;
                        line[next] += rightWeight * value;
                    };
                    tintSplat(redPlane, static_cast<uint32_t>(r));
                    tintSplat(greenPlane, static_cast<uint32_t>(g));
                    tintSplat(bluePlane, static_cast<uint32_t>(b));
                    splat(lumaPlane, level);
                } else if (flags.luma) {
                    splat(lumaPlane, luma709(r, g, b));
                }
            }
        }
    }
    mapBinsToImage(sampledRows);
}

std::optional<NormalizedPoint> Waveform::project(const FloatColor& color) const
{
    const float luma = (54.0f * color.r + 183.0f * color.g + 19.0f * color.b) / 256.0f;
    return NormalizedPoint{-1.0f, (255.0f - luma) / 255.0f};
}

void Waveform::mapBinsToImage(uint64_t sampledRows)
{
    correctBinDensities();
    const std::vector<uint32_t>& traces = m_smoothed;
    const std::size_t planeSize = this->planeSize();

    const ModeFlags flags = modeFlagsFor(m_settings.mode);
    const uint32_t densest = peakDensity(traces, planeSize, flags.rgb, flags.luma);

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
    m_smoothed.resize(m_bins.size());
    m_corrected.resize(planeSize);
    for (int plane = 0; plane < 4; ++plane) {
        const uint32_t* in = m_bins.data() + static_cast<std::size_t>(plane) * planeSize;
        uint32_t* out = m_smoothed.data() + static_cast<std::size_t>(plane) * planeSize;

        uint64_t global[Levels] = {};
        sumLevelDensities(in, m_columns, global);
        const PopulatedRange range = populatedRange(global);

        uint32_t flatten[Levels];
        double expectedOf[Levels];
        computeFlattenWeights(global, range.lowest, range.highest, flatten, expectedOf);

        int mixAbove[Levels];
        int mixBelow[Levels];
        computeMixTargets(global, expectedOf, range.lowest, range.highest, mixAbove, mixBelow);

        applyCorrection(in, m_columns, flatten, mixAbove, mixBelow, m_corrected.data());
        smoothPlane(m_corrected.data(), m_columns, out);
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
            const uint32_t* sourceRow = planes[channel] + static_cast<std::size_t>(row) * m_columns;
            uint32_t* outRow = outPlane + static_cast<std::size_t>(row) * m_columns;
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
    const ModeFlags flags = modeFlagsFor(m_settings.mode);
    const WaveformPlanes planes{redPlane, greenPlane, bluePlane, lumaPlane};

    // The composer. At native height rows map one-to-one onto levels; a
    // taller image samples the level axis through a Catmull-Rom spline -
    // the histogram's technique - so a magnified trace draws as a curve
    // instead of stretched texels.
    const bool nativeHeight = m_imageHeight == Levels;
    uint8_t* out = m_image.rgba.data();
    for (int y = 0; y < m_imageHeight; ++y) {
        const LevelSample tap = levelSampleWeights(y, m_imageHeight, nativeHeight);
        const auto sample = [&](const uint32_t* plane, int column) -> float {
            const auto rowAt = [&](int level) -> float {
                if (level < 0 || level >= Levels) {
                    return 0.0f;
                }
                return static_cast<float>(plane[static_cast<std::size_t>(level) * m_columns + column]);
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
}

}  // namespace sidescopes
