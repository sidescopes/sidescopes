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
                const std::size_t column =
                    static_cast<std::size_t>(static_cast<int64_t>(px) * kColumns / region.width);
                if (wants_rgb) {
                    ++red_plane[static_cast<std::size_t>(255 - r) * kColumns + column];
                    ++green_plane[static_cast<std::size_t>(255 - g) * kColumns + column];
                    ++blue_plane[static_cast<std::size_t>(255 - b) * kColumns + column];
                }
                if (wants_luma) {
                    const int luma = Luma709(r, g, b);
                    ++luma_plane[static_cast<std::size_t>(255 - luma) * kColumns + column];
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
    const bool wants_rgb = settings_.mode != WaveformMode::Luma;
    const bool wants_luma =
        settings_.mode == WaveformMode::Luma || settings_.mode == WaveformMode::RgbAndLuma;

    uint32_t densest = 0;
    if (wants_rgb) {
        for (std::size_t i = 0; i < 3 * kPlaneSize; ++i) densest = std::max(densest, bins_[i]);
    }
    if (wants_luma) {
        for (std::size_t i = 3 * kPlaneSize; i < 4 * kPlaneSize; ++i)
            densest = std::max(densest, bins_[i]);
    }

    const double per_row_scale =
        sampled_rows > 0 ? kReferenceRowCount / static_cast<double>(sampled_rows) : 0.0;
    const double gain = static_cast<double>(settings_.gain) * per_row_scale;
    const double log_ceiling = densest > 0 ? std::log1p(static_cast<double>(densest) * gain) : 0.0;
    const double intensity_scale = log_ceiling > 0.0 ? 255.0 / log_ceiling : 0.0;
    const auto brightness = [&](uint32_t count) -> float {
        if (count == 0) return 0.0f;
        return static_cast<float>(std::log1p(static_cast<double>(count) * gain) * intensity_scale);
    };

    const uint32_t* red_plane = bins_.data();
    const uint32_t* green_plane = bins_.data() + kPlaneSize;
    const uint32_t* blue_plane = bins_.data() + 2 * kPlaneSize;
    const uint32_t* luma_plane = bins_.data() + 3 * kPlaneSize;

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
