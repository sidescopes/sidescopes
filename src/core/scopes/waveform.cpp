#include "core/scopes/waveform.h"

#include <algorithm>
#include <cmath>

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

    const bool coloredLuma = m_settings.mode == WaveformMode::ColoredLuma;
    const bool wantsRgb = m_settings.mode != WaveformMode::Luma && !coloredLuma;
    const bool wantsLuma =
        m_settings.mode == WaveformMode::Luma || m_settings.mode == WaveformMode::RgbAndLuma || coloredLuma;
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
                if (wantsRgb) {
                    splat(redPlane, r);
                    splat(greenPlane, g);
                    splat(bluePlane, b);
                }
                if (coloredLuma) {
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
                } else if (wantsLuma) {
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
    for (int plane = 0; plane < 4; ++plane) {
        const uint32_t* in = m_bins.data() + static_cast<std::size_t>(plane) * planeSize;
        uint32_t* out = m_smoothed.data() + static_cast<std::size_t>(plane) * planeSize;

        uint64_t global[Levels] = {};
        for (int row = 0; row < Levels; ++row) {
            const uint32_t* line = in + static_cast<std::size_t>(row) * m_columns;
            for (int column = 0; column < m_columns; ++column) {
                global[row] += line[column];
            }
        }
        // The populated range: spikes at its edges are real clipping
        // lines - crushed blacks, blown whites - and stay protected.
        int lowest = Levels;
        int highest = -1;
        for (int row = 0; row < Levels; ++row) {
            if (global[row] == 0) {
                continue;
            }
            if (lowest == Levels) {
                lowest = row;
            }
            highest = row;
        }

        // Flat-field weights in 1/256ths: the neighborhood MEDIAN over
        // +-6 levels against the level's own population. The median
        // matters: a genuinely dominant flat tone is a huge real spike,
        // and a mean would inflate its neighbors' expected density and
        // over-lift them - manufacturing the very banding this removes.
        uint32_t flatten[Levels];
        double expectedOf[Levels];
        for (int row = 0; row < Levels; ++row) {
            uint64_t neighborhood[12];
            int counted = 0;
            for (int near = row - 6; near <= row + 6; ++near) {
                if (near == row || near < 0 || near >= Levels) {
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
            const double expected = counted > 0 ? static_cast<double>(neighborhood[middle]) : 0.0;
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

        // Codes the pipeline never emits cannot be lifted by weighting -
        // their counts are zero. They are reconstructed instead: a
        // starved interior code takes the distance-weighted mix of its
        // nearest healthy neighbors, per column, so the trace reads as
        // the continuous signal the display quantized away. Only short
        // gaps qualify; wider ones are honest emptiness relative to
        // their neighborhood and stay dark.
        const auto healthy = [&](int row) {
            return expectedOf[row] <= 0.0 || static_cast<double>(global[row]) >= expectedOf[row] * 0.25;
        };
        int mixAbove[Levels];
        int mixBelow[Levels];
        for (int row = 0; row < Levels; ++row) {
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
            for (int near = row + 1; near <= row + 3 && near < Levels; ++near) {
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

        m_corrected.resize(planeSize);
        for (int row = 0; row < Levels; ++row) {
            uint32_t* line = m_corrected.data() + static_cast<std::size_t>(row) * m_columns;
            const auto weighted = [&](int level, int column) -> uint32_t {
                const uint64_t count = in[static_cast<std::size_t>(level) * m_columns + column];
                return static_cast<uint32_t>(count * flatten[level] >> 8);
            };
            if (mixAbove[row] == row) {
                for (int column = 0; column < m_columns; ++column) {
                    line[column] = weighted(row, column);
                }
            } else {
                const int above = mixAbove[row];
                const int below = mixBelow[row];
                const uint32_t gap = static_cast<uint32_t>(below - above);
                const uint32_t belowShare = static_cast<uint32_t>(row - above);
                const uint32_t aboveShare = static_cast<uint32_t>(below - row);
                for (int column = 0; column < m_columns; ++column) {
                    line[column] = (weighted(above, column) * aboveShare + weighted(below, column) * belowShare) / gap;
                }
            }
        }

        // Vertical 1-4-1: light, so a sharp level stays crisp while
        // single-bin grain still fills in. The banding work lives in the
        // flat-field and the dead-code reconstruction above - a wider
        // kernel here only blurred what they had already repaired, and
        // big panes magnified that blur.
        for (int column = 0; column < m_columns; ++column) {
            for (int row = 0; row < Levels; ++row) {
                const auto at = [&](int level) -> uint32_t {
                    if (level < 0 || level >= Levels) {
                        return 0;
                    }
                    return m_corrected[static_cast<std::size_t>(level) * m_columns + column];
                };
                out[static_cast<std::size_t>(row) * m_columns + column] =
                    (at(row - 1) + 4 * at(row) + at(row + 1) + 3) / 6;
            }
        }
        // Horizontal 1-2-1 within each row, in place.
        for (int row = 0; row < Levels; ++row) {
            uint32_t* line = out + static_cast<std::size_t>(row) * m_columns;
            uint32_t previous = 0;
            for (int column = 0; column < m_columns; ++column) {
                const uint32_t current = line[column];
                const uint32_t next = column + 1 < m_columns ? line[column + 1] : 0;
                line[column] = (previous + 2 * current + next + 2) / 4;
                previous = current;
            }
        }
    }
    const std::vector<uint32_t>& traces = m_smoothed;

    const bool coloredLuma = m_settings.mode == WaveformMode::ColoredLuma;
    const bool wantsRgb = m_settings.mode != WaveformMode::Luma && !coloredLuma;
    const bool wantsLuma =
        m_settings.mode == WaveformMode::Luma || m_settings.mode == WaveformMode::RgbAndLuma || coloredLuma;

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

    // Each sample contributes sixteen weight units (the splat's
    // sixteenths), so the per-row normalization divides them back out and
    // the gain setting keeps its calibrated feel.
    const double perRowScale = sampledRows > 0 ? ReferenceRowCount / (static_cast<double>(sampledRows) * 16.0) : 0.0;
    const double gain = static_cast<double>(m_settings.gain) * perRowScale;
    const double logCeiling = densest > 0 ? std::log1p(static_cast<double>(densest) * gain) : 0.0;
    const double intensityScale = logCeiling > 0.0 ? 255.0 / logCeiling : 0.0;
    const auto brightness = [&](float count) -> float {
        if (count <= 0.0f) {
            return 0.0f;
        }
        // The gamma lifts the mid-density body of the trace, exactly as
        // on the vectorscope: normalizing to the densest bin pushes
        // everything else down, and a linear ramp reads dim at any gain.
        const double normalized = std::log1p(static_cast<double>(count) * gain) * intensityScale / 255.0;
        return static_cast<float>(255.0 * std::pow(normalized, 0.65));
    };

    const uint32_t* redPlane = traces.data();
    const uint32_t* greenPlane = traces.data() + planeSize;
    const uint32_t* bluePlane = traces.data() + 2 * planeSize;
    const uint32_t* lumaPlane = traces.data() + 3 * planeSize;

    if (m_settings.mode == WaveformMode::RgbParade) {
        // Three channels side by side: each third shows one channel's
        // full column range compressed 3:1, window-maxed so sparse
        // traces stay visible. The result feeds the same composer as
        // the overlaid modes.
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
        redPlane = m_parade.data();
        greenPlane = m_parade.data() + planeSize;
        bluePlane = m_parade.data() + 2 * planeSize;
    }

    // The composer. At native height rows map one-to-one onto levels; a
    // taller image samples the level axis through a Catmull-Rom spline -
    // the histogram's technique - so a magnified trace draws as a curve
    // instead of stretched texels.
    const bool nativeHeight = m_imageHeight == Levels;
    uint8_t* out = m_image.rgba.data();
    for (int y = 0; y < m_imageHeight; ++y) {
        int base = y;
        float weight0 = 0.0f;
        float weight1 = 1.0f;
        float weight2 = 0.0f;
        float weight3 = 0.0f;
        if (!nativeHeight) {
            const float position = (static_cast<float>(y) + 0.5f) * Levels / static_cast<float>(m_imageHeight) - 0.5f;
            const float floored = std::floor(position);
            base = static_cast<int>(floored);
            const float t = position - floored;
            const float t2 = t * t;
            const float t3 = t2 * t;
            weight0 = -0.5f * t3 + t2 - 0.5f * t;
            weight1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
            weight2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
            weight3 = 0.5f * t3 - 0.5f * t2;
        }
        const auto sample = [&](const uint32_t* plane, int column) -> float {
            const auto rowAt = [&](int level) -> float {
                if (level < 0 || level >= Levels) {
                    return 0.0f;
                }
                return static_cast<float>(plane[static_cast<std::size_t>(level) * m_columns + column]);
            };
            if (nativeHeight) {
                return rowAt(base);
            }
            return std::max(0.0f, weight0 * rowAt(base - 1) + weight1 * rowAt(base) + weight2 * rowAt(base + 1) +
                                      weight3 * rowAt(base + 2));
        };
        for (int column = 0; column < m_columns; ++column, out += 4) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            if (wantsRgb) {
                r = brightness(sample(redPlane, column));
                g = brightness(sample(greenPlane, column));
                b = brightness(sample(bluePlane, column));
            }
            if (coloredLuma) {
                // Density decides how bright the trace is; the
                // value-weighted planes only decide its color, so a
                // dense shadow region draws as clearly as a dense
                // highlight, each in its own tint.
                const float density = brightness(sample(lumaPlane, column));
                const float massR = sample(redPlane, column);
                const float massG = sample(greenPlane, column);
                const float massB = sample(bluePlane, column);
                const float strongest = std::max({massR, massG, massB});
                if (strongest > 0.0f) {
                    r = density * (massR / strongest);
                    g = density * (massG / strongest);
                    b = density * (massB / strongest);
                } else {
                    r = g = b = density;
                }
            } else if (wantsLuma) {
                // In the combined mode luma rides on top as a dimmer
                // white trace.
                const float luma = brightness(sample(lumaPlane, column)) * (wantsRgb ? 0.7f : 1.0f);
                r += luma;
                g += luma;
                b += luma;
            }
            out[0] = static_cast<uint8_t>(std::min(255.0f, r));
            out[1] = static_cast<uint8_t>(std::min(255.0f, g));
            out[2] = static_cast<uint8_t>(std::min(255.0f, b));
            out[3] = 255;
        }
    }
    ++m_image.sequence;
}

}  // namespace sidescopes
