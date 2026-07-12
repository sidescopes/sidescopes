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

Waveform::Waveform() {
    Resize(kDefaultWaveformColumns, kWaveformLevels);
}

void Waveform::Configure(const WaveformSettings& settings) {
    settings_ = settings;
    settings_.sampling_stride = std::clamp(settings_.sampling_stride, 1, 8);
    settings_.columns = std::clamp(settings_.columns, 256, 2048);
    settings_.image_height = std::clamp(settings_.image_height, kWaveformLevels, 768);
    if (settings_.columns != columns_ || settings_.image_height != image_height_)
        Resize(settings_.columns, settings_.image_height);
}

void Waveform::Resize(int columns, int image_height) {
    columns_ = columns;
    image_height_ = image_height;
    bins_.assign(PlaneSize() * 4, 0);
    image_.width = columns_;
    image_.height = image_height_;
    image_.rgba.assign(static_cast<std::size_t>(columns_) * image_height_ * 4, 0);
}

void Waveform::Accumulate(const FrameView& frame, IntRect region) {
    region = region.ClampedTo(frame.width, frame.height);
    std::fill(bins_.begin(), bins_.end(), 0u);

    const bool colored_luma = settings_.mode == WaveformMode::ColoredLuma;
    const bool wants_rgb = settings_.mode != WaveformMode::Luma && !colored_luma;
    const bool wants_luma = settings_.mode == WaveformMode::Luma ||
                            settings_.mode == WaveformMode::RgbAndLuma || colored_luma;
    const std::size_t plane_size = PlaneSize();
    uint32_t* red_plane = bins_.data();
    uint32_t* green_plane = bins_.data() + plane_size;
    uint32_t* blue_plane = bins_.data() + 2 * plane_size;
    uint32_t* luma_plane = bins_.data() + 3 * plane_size;

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
                const auto position = static_cast<std::size_t>(static_cast<int64_t>(px) * columns_ *
                                                               16 / region.width);
                const std::size_t column = position >> 4;
                const uint32_t right_weight = position & 15u;
                const uint32_t left_weight = 16u - right_weight;
                const std::size_t next =
                    column + 1 < static_cast<std::size_t>(columns_) ? column + 1 : column;
                const auto splat = [&](uint32_t* plane, int value) {
                    uint32_t* line = plane + static_cast<std::size_t>(255 - value) * columns_;
                    line[column] += left_weight;
                    line[next] += right_weight;
                };
                if (wants_rgb) {
                    splat(red_plane, r);
                    splat(green_plane, g);
                    splat(blue_plane, b);
                }
                if (colored_luma) {
                    // The luma plane carries the density; the channel
                    // planes carry value-weighted mass at the same rows,
                    // so each cell remembers the average color of the
                    // pixels that landed on it.
                    const int level = Luma709(r, g, b);
                    const auto tint_splat = [&](uint32_t* plane, uint32_t value) {
                        uint32_t* line = plane + static_cast<std::size_t>(255 - level) * columns_;
                        line[column] += left_weight * value;
                        line[next] += right_weight * value;
                    };
                    tint_splat(red_plane, static_cast<uint32_t>(r));
                    tint_splat(green_plane, static_cast<uint32_t>(g));
                    tint_splat(blue_plane, static_cast<uint32_t>(b));
                    splat(luma_plane, level);
                } else if (wants_luma) {
                    splat(luma_plane, Luma709(r, g, b));
                }
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
    const std::size_t plane_size = PlaneSize();
    smoothed_.resize(bins_.size());
    for (int plane = 0; plane < 4; ++plane) {
        const uint32_t* in = bins_.data() + static_cast<std::size_t>(plane) * plane_size;
        uint32_t* out = smoothed_.data() + static_cast<std::size_t>(plane) * plane_size;

        uint64_t global[kLevels] = {};
        for (int row = 0; row < kLevels; ++row) {
            const uint32_t* line = in + static_cast<std::size_t>(row) * columns_;
            for (int column = 0; column < columns_; ++column) global[row] += line[column];
        }
        // The populated range: spikes at its edges are real clipping
        // lines - crushed blacks, blown whites - and stay protected.
        int lowest = kLevels;
        int highest = -1;
        for (int row = 0; row < kLevels; ++row) {
            if (global[row] == 0) continue;
            if (lowest == kLevels) lowest = row;
            highest = row;
        }

        // Flat-field weights in 1/256ths: the neighborhood MEDIAN over
        // +-6 levels against the level's own population. The median
        // matters: a genuinely dominant flat tone is a huge real spike,
        // and a mean would inflate its neighbors' expected density and
        // over-lift them - manufacturing the very banding this removes.
        uint32_t flatten[kLevels];
        double expected_of[kLevels];
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
            expected_of[row] = expected;

            // A pipeline pileup steals its mass from nearby codes, so it
            // always travels with starved neighbors inside the populated
            // range; a real flat tone starves nothing, and a real
            // clipping line sits at the range's edge. Only the pileups
            // may be attenuated without limit.
            bool starved_nearby = false;
            if (expected > 0.0) {
                for (int near = row - 4; near <= row + 4; ++near) {
                    if (near == row || near <= lowest + 1 || near >= highest - 1) continue;
                    if (static_cast<double>(global[near]) < expected * 0.1) starved_nearby = true;
                }
            }
            const bool interior = row > lowest + 2 && row < highest - 2;
            const double attenuation_floor = (starved_nearby && interior) ? 1.0 / 64.0 : 1.0 / 3.0;

            double weight = 1.0;
            if (global[row] > 0 && expected > 0.0)
                weight =
                    std::clamp(expected / static_cast<double>(global[row]), attenuation_floor, 3.0);
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
            return expected_of[row] <= 0.0 ||
                   static_cast<double>(global[row]) >= expected_of[row] * 0.25;
        };
        int mix_above[kLevels];
        int mix_below[kLevels];
        for (int row = 0; row < kLevels; ++row) {
            mix_above[row] = row;
            mix_below[row] = row;
            const bool interior = row > lowest + 2 && row < highest - 2;
            if (!interior || healthy(row)) continue;
            int above = -1;
            for (int near = row - 1; near >= row - 3 && near >= 0; --near) {
                if (healthy(near)) {
                    above = near;
                    break;
                }
            }
            int below = -1;
            for (int near = row + 1; near <= row + 3 && near < kLevels; ++near) {
                if (healthy(near)) {
                    below = near;
                    break;
                }
            }
            if (above >= 0 && below >= 0) {
                mix_above[row] = above;
                mix_below[row] = below;
            }
        }

        corrected_.resize(plane_size);
        for (int row = 0; row < kLevels; ++row) {
            uint32_t* line = corrected_.data() + static_cast<std::size_t>(row) * columns_;
            const auto weighted = [&](int level, int column) -> uint32_t {
                const uint64_t count = in[static_cast<std::size_t>(level) * columns_ + column];
                return static_cast<uint32_t>(count * flatten[level] >> 8);
            };
            if (mix_above[row] == row) {
                for (int column = 0; column < columns_; ++column)
                    line[column] = weighted(row, column);
            } else {
                const int above = mix_above[row];
                const int below = mix_below[row];
                const uint32_t gap = static_cast<uint32_t>(below - above);
                const uint32_t below_share = static_cast<uint32_t>(row - above);
                const uint32_t above_share = static_cast<uint32_t>(below - row);
                for (int column = 0; column < columns_; ++column)
                    line[column] = (weighted(above, column) * above_share +
                                    weighted(below, column) * below_share) /
                                   gap;
            }
        }

        // Vertical 1-4-1: light, so a sharp level stays crisp while
        // single-bin grain still fills in. The banding work lives in the
        // flat-field and the dead-code reconstruction above - a wider
        // kernel here only blurred what they had already repaired, and
        // big panes magnified that blur.
        for (int column = 0; column < columns_; ++column) {
            for (int row = 0; row < kLevels; ++row) {
                const auto at = [&](int level) -> uint32_t {
                    if (level < 0 || level >= kLevels) return 0;
                    return corrected_[static_cast<std::size_t>(level) * columns_ + column];
                };
                out[static_cast<std::size_t>(row) * columns_ + column] =
                    (at(row - 1) + 4 * at(row) + at(row + 1) + 3) / 6;
            }
        }
        // Horizontal 1-2-1 within each row, in place.
        for (int row = 0; row < kLevels; ++row) {
            uint32_t* line = out + static_cast<std::size_t>(row) * columns_;
            uint32_t previous = 0;
            for (int column = 0; column < columns_; ++column) {
                const uint32_t current = line[column];
                const uint32_t next = column + 1 < columns_ ? line[column + 1] : 0;
                line[column] = (previous + 2 * current + next + 2) / 4;
                previous = current;
            }
        }
    }
    const std::vector<uint32_t>& traces = smoothed_;

    const bool colored_luma = settings_.mode == WaveformMode::ColoredLuma;
    const bool wants_rgb = settings_.mode != WaveformMode::Luma && !colored_luma;
    const bool wants_luma = settings_.mode == WaveformMode::Luma ||
                            settings_.mode == WaveformMode::RgbAndLuma || colored_luma;

    uint32_t densest = 0;
    if (wants_rgb) {
        for (std::size_t i = 0; i < 3 * plane_size; ++i) densest = std::max(densest, traces[i]);
    }
    if (wants_luma) {
        for (std::size_t i = 3 * plane_size; i < 4 * plane_size; ++i)
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
    const auto brightness = [&](float count) -> float {
        if (count <= 0.0f) return 0.0f;
        // The gamma lifts the mid-density body of the trace, exactly as
        // on the vectorscope: normalizing to the densest bin pushes
        // everything else down, and a linear ramp reads dim at any gain.
        const double normalized =
            std::log1p(static_cast<double>(count) * gain) * intensity_scale / 255.0;
        return static_cast<float>(255.0 * std::pow(normalized, 0.65));
    };

    const uint32_t* red_plane = traces.data();
    const uint32_t* green_plane = traces.data() + plane_size;
    const uint32_t* blue_plane = traces.data() + 2 * plane_size;
    const uint32_t* luma_plane = traces.data() + 3 * plane_size;

    if (settings_.mode == WaveformMode::RgbParade) {
        // Three channels side by side: each third shows one channel's
        // full column range compressed 3:1, window-maxed so sparse
        // traces stay visible. The result feeds the same composer as
        // the overlaid modes.
        parade_.assign(3 * plane_size, 0);
        const int third = columns_ / 3;
        const uint32_t* planes[3] = {red_plane, green_plane, blue_plane};
        for (int channel = 0; channel < 3; ++channel) {
            uint32_t* out_plane = parade_.data() + static_cast<std::size_t>(channel) * plane_size;
            const int first = channel * third;
            const int last = channel == 2 ? columns_ : (channel + 1) * third;
            for (int row = 0; row < kLevels; ++row) {
                const uint32_t* source_row =
                    planes[channel] + static_cast<std::size_t>(row) * columns_;
                uint32_t* out_row = out_plane + static_cast<std::size_t>(row) * columns_;
                for (int column = first; column < last; ++column) {
                    const int local = column - first;
                    const int begin = local * columns_ / third;
                    const int end = std::min((local + 1) * columns_ / third, columns_);
                    uint32_t densest_in_window = 0;
                    for (int source = begin; source < end; ++source)
                        densest_in_window = std::max(densest_in_window, source_row[source]);
                    out_row[column] = densest_in_window;
                }
            }
        }
        red_plane = parade_.data();
        green_plane = parade_.data() + plane_size;
        blue_plane = parade_.data() + 2 * plane_size;
    }

    // The composer. At native height rows map one-to-one onto levels; a
    // taller image samples the level axis through a Catmull-Rom spline -
    // the histogram's technique - so a magnified trace draws as a curve
    // instead of stretched texels.
    const bool native_height = image_height_ == kLevels;
    uint8_t* out = image_.rgba.data();
    for (int y = 0; y < image_height_; ++y) {
        int base = y;
        float weight0 = 0.0f;
        float weight1 = 1.0f;
        float weight2 = 0.0f;
        float weight3 = 0.0f;
        if (!native_height) {
            const float position =
                (static_cast<float>(y) + 0.5f) * kLevels / static_cast<float>(image_height_) - 0.5f;
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
            const auto row_at = [&](int level) -> float {
                if (level < 0 || level >= kLevels) return 0.0f;
                return static_cast<float>(
                    plane[static_cast<std::size_t>(level) * columns_ + column]);
            };
            if (native_height) return row_at(base);
            return std::max(0.0f, weight0 * row_at(base - 1) + weight1 * row_at(base) +
                                      weight2 * row_at(base + 1) + weight3 * row_at(base + 2));
        };
        for (int column = 0; column < columns_; ++column, out += 4) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            if (wants_rgb) {
                r = brightness(sample(red_plane, column));
                g = brightness(sample(green_plane, column));
                b = brightness(sample(blue_plane, column));
            }
            if (colored_luma) {
                // Density decides how bright the trace is; the
                // value-weighted planes only decide its color, so a
                // dense shadow region draws as clearly as a dense
                // highlight, each in its own tint.
                const float density = brightness(sample(luma_plane, column));
                const float mass_r = sample(red_plane, column);
                const float mass_g = sample(green_plane, column);
                const float mass_b = sample(blue_plane, column);
                const float strongest = std::max({mass_r, mass_g, mass_b});
                if (strongest > 0.0f) {
                    r = density * (mass_r / strongest);
                    g = density * (mass_g / strongest);
                    b = density * (mass_b / strongest);
                } else {
                    r = g = b = density;
                }
            } else if (wants_luma) {
                // In the combined mode luma rides on top as a dimmer
                // white trace.
                const float luma =
                    brightness(sample(luma_plane, column)) * (wants_rgb ? 0.7f : 1.0f);
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
    ++image_.sequence;
}

}  // namespace sidescopes
