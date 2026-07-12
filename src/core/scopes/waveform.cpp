#include "core/scopes/waveform.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

// Rec.709 luma weights, fixed-point x256, applied to display-encoded values.
inline int Luma709(int r, int g, int b) {
    return (54 * r + 183 * g + 19 * b) >> 8;
}

// A waveform column is populated by one sample per sampled row, so densities
// are normalized per sampled row: column brightness is then invariant to the
// sampling stride and to the region size.
constexpr double kReferenceRowCount = 1'000.0;

}  // namespace

Waveform::Waveform() : bins_(kPlaneSize * 4, 0) {
    image_.width = kColumns;
    image_.height = kLevels;
    image_.rgba.assign(kPlaneSize * 4, 0);
}

void Waveform::Configure(const WaveformSettings& settings) {
    settings_ = settings;
    settings_.sampling_stride = std::clamp(settings_.sampling_stride, 1, 8);
}

void Waveform::Accumulate(const FrameView& frame, IntRect region) {
    region = region.ClampedTo(frame.width, frame.height);
    std::fill(bins_.begin(), bins_.end(), 0u);

    const bool wants_rgb = settings_.mode != WaveformMode::Luma;
    const bool wants_luma =
        settings_.mode == WaveformMode::Luma || settings_.mode == WaveformMode::RgbAndLuma;
    uint32_t* red_plane = bins_.data();
    uint32_t* green_plane = bins_.data() + kPlaneSize;
    uint32_t* blue_plane = bins_.data() + 2 * kPlaneSize;
    uint32_t* luma_plane = bins_.data() + 3 * kPlaneSize;

    uint64_t sampled_rows = 0;
    if (!region.Empty()) {
        const int stride = settings_.sampling_stride;
        for (int py = region.y; py < region.y + region.height; py += stride) {
            ++sampled_rows;
            const uint8_t* row = frame.PixelAt(region.x, py);
            for (int px = 0; px < region.width; px += stride) {
                const uint8_t* pixel = row + static_cast<std::size_t>(px) * 4;
                const int b = pixel[0], g = pixel[1], r = pixel[2];
                // Samples splat fractionally across the two columns they
                // straddle, in sixteenths. Integer bucketing made columns
                // aggregate alternately two and three image columns at
                // typical region widths - a density comb that rendered as
                // fine vertical striping on large panes.
                const auto position = static_cast<std::size_t>(static_cast<int64_t>(px) * kColumns *
                                                               16 / region.width);
                const std::size_t column = position >> 4;
                const uint32_t right_weight = position & 15u;
                const uint32_t left_weight = 16u - right_weight;
                const std::size_t next =
                    column + 1 < static_cast<std::size_t>(kColumns) ? column + 1 : column;
                const auto splat = [&](uint32_t* plane, int value) {
                    uint32_t* line = plane + static_cast<std::size_t>(255 - value) * kColumns;
                    line[column] += left_weight;
                    line[next] += right_weight;
                };
                if (wants_rgb) {
                    splat(red_plane, r);
                    splat(green_plane, g);
                    splat(blue_plane, b);
                }
                if (wants_luma) splat(luma_plane, Luma709(r, g, b));
            }
        }
    }
    MapBinsToImage(sampled_rows);
}

std::optional<NormalizedPoint> Waveform::Project(const FloatColor& color) const {
    const float luma = (54.0f * color.r + 183.0f * color.g + 19.0f * color.b) / 256.0f;
    return NormalizedPoint{-1.0f, (255.0f - luma) / 255.0f};
}

void Waveform::MapBinsToImage(uint64_t sampled_rows) {
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
    smoothed_.resize(bins_.size());
    for (int plane = 0; plane < 4; ++plane) {
        const uint32_t* in = bins_.data() + static_cast<std::size_t>(plane) * kPlaneSize;
        uint32_t* out = smoothed_.data() + static_cast<std::size_t>(plane) * kPlaneSize;

        uint64_t global[kLevels] = {};
        for (int row = 0; row < kLevels; ++row) {
            const uint32_t* line = in + static_cast<std::size_t>(row) * kColumns;
            for (int column = 0; column < kColumns; ++column) global[row] += line[column];
        }
        // Flat-field weights in 1/256ths: the neighborhood MEDIAN over
        // +-6 levels against the level's own population, clamped to 3x.
        // The median matters: a genuinely dominant flat tone is a huge
        // real spike, and a mean would inflate its neighbors' expected
        // density and over-lift them - manufacturing the very banding
        // this removes. The median ignores isolated spikes, so real
        // lines keep their real neighbors.
        uint32_t flatten[kLevels];
        for (int row = 0; row < kLevels; ++row) {
            uint64_t neighborhood[12];
            int counted = 0;
            for (int near = row - 6; near <= row + 6; ++near) {
                if (near == row || near < 0 || near >= kLevels) continue;
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
            double weight = 1.0;
            if (global[row] > 0 && expected > 0.0)
                weight = std::clamp(expected / static_cast<double>(global[row]), 1.0 / 3.0, 3.0);
            flatten[row] = static_cast<uint32_t>(weight * 256.0);
        }

        for (int column = 0; column < kColumns; ++column) {
            for (int row = 0; row < kLevels; ++row) {
                const auto at = [&](int level) -> uint32_t {
                    if (level < 0 || level >= kLevels) return 0;
                    const uint64_t count = in[static_cast<std::size_t>(level) * kColumns + column];
                    return static_cast<uint32_t>(count * flatten[level] >> 8);
                };
                out[static_cast<std::size_t>(row) * kColumns + column] =
                    (at(row - 2) + 4 * at(row - 1) + 6 * at(row) + 4 * at(row + 1) + at(row + 2) +
                     8) /
                    16;
            }
        }
        // Horizontal 1-2-1 within each row, in place.
        for (int row = 0; row < kLevels; ++row) {
            uint32_t* line = out + static_cast<std::size_t>(row) * kColumns;
            uint32_t previous = 0;
            for (int column = 0; column < kColumns; ++column) {
                const uint32_t current = line[column];
                const uint32_t next = column + 1 < kColumns ? line[column + 1] : 0;
                line[column] = (previous + 2 * current + next + 2) / 4;
                previous = current;
            }
        }
    }
    const std::vector<uint32_t>& traces = smoothed_;

    const bool wants_rgb = settings_.mode != WaveformMode::Luma;
    const bool wants_luma =
        settings_.mode == WaveformMode::Luma || settings_.mode == WaveformMode::RgbAndLuma;

    uint32_t densest = 0;
    if (wants_rgb) {
        for (std::size_t i = 0; i < 3 * kPlaneSize; ++i) densest = std::max(densest, traces[i]);
    }
    if (wants_luma) {
        for (std::size_t i = 3 * kPlaneSize; i < 4 * kPlaneSize; ++i)
            densest = std::max(densest, traces[i]);
    }

    // Each sample contributes sixteen weight units (the splat's
    // sixteenths), so the per-row normalization divides them back out and
    // the gain setting keeps its calibrated feel.
    const double per_row_scale =
        sampled_rows > 0 ? kReferenceRowCount / (static_cast<double>(sampled_rows) * 16.0) : 0.0;
    const double gain = static_cast<double>(settings_.gain) * per_row_scale;
    const double log_ceiling = densest > 0 ? std::log1p(static_cast<double>(densest) * gain) : 0.0;
    const double intensity_scale = log_ceiling > 0.0 ? 255.0 / log_ceiling : 0.0;
    const auto brightness = [&](uint32_t count) -> float {
        if (count == 0) return 0.0f;
        // The gamma lifts the mid-density body of the trace, exactly as
        // on the vectorscope: normalizing to the densest bin pushes
        // everything else down, and a linear ramp reads dim at any gain.
        const double normalized =
            std::log1p(static_cast<double>(count) * gain) * intensity_scale / 255.0;
        return static_cast<float>(255.0 * std::pow(normalized, 0.65));
    };

    const uint32_t* red_plane = traces.data();
    const uint32_t* green_plane = traces.data() + kPlaneSize;
    const uint32_t* blue_plane = traces.data() + 2 * kPlaneSize;
    const uint32_t* luma_plane = traces.data() + 3 * kPlaneSize;

    if (settings_.mode == WaveformMode::RgbParade) {
        // Three channels side by side: each third shows one channel's full
        // column range compressed 3:1.
        const uint32_t* planes[3] = {red_plane, green_plane, blue_plane};
        constexpr int kThird = kColumns / 3;
        uint8_t* out = image_.rgba.data();
        for (int row = 0; row < kLevels; ++row) {
            for (int column = 0; column < kColumns; ++column, out += 4) {
                const int channel = std::min(column / kThird, 2);
                const int local = column - channel * kThird;
                // Each output column covers a window of source columns; the
                // window maximum keeps sparse traces visible (narrow regions
                // populate only every Nth source column).
                const int source_begin = local * kColumns / kThird;
                const int source_end = std::min((local + 1) * kColumns / kThird, kColumns);
                uint32_t densest_in_window = 0;
                for (int source = source_begin; source < source_end; ++source) {
                    densest_in_window = std::max(
                        densest_in_window,
                        planes[channel][static_cast<std::size_t>(row) * kColumns + source]);
                }
                const float value = brightness(densest_in_window);
                out[0] = channel == 0 ? static_cast<uint8_t>(std::min(255.0f, value)) : 0;
                out[1] = channel == 1 ? static_cast<uint8_t>(std::min(255.0f, value)) : 0;
                out[2] = channel == 2 ? static_cast<uint8_t>(std::min(255.0f, value)) : 0;
                out[3] = 255;
            }
        }
        ++image_.sequence;
        return;
    }

    uint8_t* out = image_.rgba.data();
    for (std::size_t i = 0; i < kPlaneSize; ++i, out += 4) {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (wants_rgb) {
            r = brightness(red_plane[i]);
            g = brightness(green_plane[i]);
            b = brightness(blue_plane[i]);
        }
        if (wants_luma) {
            // In the combined mode luma rides on top as a dimmer white trace.
            const float luma = brightness(luma_plane[i]) * (wants_rgb ? 0.7f : 1.0f);
            r += luma;
            g += luma;
            b += luma;
        }
        out[0] = static_cast<uint8_t>(std::min(255.0f, r));
        out[1] = static_cast<uint8_t>(std::min(255.0f, g));
        out[2] = static_cast<uint8_t>(std::min(255.0f, b));
        out[3] = 255;
    }
    ++image_.sequence;
}

}  // namespace sidescopes
