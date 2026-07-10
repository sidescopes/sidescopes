#include "core/scopes/vectorscope.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

// Fixed-point (x256) full-range RGB -> Cb/Cr coefficients from the BT.601
// and BT.709 specifications. Chroma lands in [0, 255] with neutral at 128.
struct ChromaCoefficients {
    int cb_from_r, cb_from_g, cb_from_b;
    int cr_from_r, cr_from_g, cr_from_b;
};

constexpr ChromaCoefficients kBt601{-38, -74, 112, 112, -94, -18};
constexpr ChromaCoefficients kBt709{-26, -87, 112, 112, -102, -10};

constexpr const ChromaCoefficients& CoefficientsFor(ChromaMatrix matrix) {
    return matrix == ChromaMatrix::Bt601 ? kBt601 : kBt709;
}

// Densities are normalized to a nominal sample count so the same scene maps
// to the same trace regardless of sampling stride or region size.
constexpr double kReferenceSampleCount = 1'000'000.0;

}  // namespace

Vectorscope::Vectorscope() : bins_(kSize * kSize, 0) {
    image_.width = kSize;
    image_.height = kSize;
    image_.rgba.assign(static_cast<std::size_t>(kSize) * kSize * 4, 0);
    RebuildTintTable();
}

void Vectorscope::Configure(const VectorscopeSettings& settings) {
    const bool matrix_changed = settings.matrix != settings_.matrix;
    settings_ = settings;
    settings_.sampling_stride = std::clamp(settings_.sampling_stride, 1, 8);
    if (matrix_changed) RebuildTintTable();
}

void Vectorscope::Accumulate(const FrameView& frame, IntRect region) {
    region = region.ClampedTo(frame.width, frame.height);
    std::fill(bins_.begin(), bins_.end(), 0u);

    uint64_t sample_count = 0;
    if (!region.Empty()) {
        const ChromaCoefficients& matrix = CoefficientsFor(settings_.matrix);
        const int stride = settings_.sampling_stride;
        for (int py = region.y; py < region.y + region.height; py += stride) {
            const uint8_t* pixel = frame.PixelAt(region.x, py);
            const uint8_t* row_end = frame.PixelAt(region.x + region.width, py);
            for (; pixel < row_end; pixel += 4 * stride) {
                const int b = pixel[0], g = pixel[1], r = pixel[2];
                const int cb =
                    ((matrix.cb_from_r * r + matrix.cb_from_g * g + matrix.cb_from_b * b) >> 8) +
                    128;
                const int cr =
                    ((matrix.cr_from_r * r + matrix.cr_from_g * g + matrix.cr_from_b * b) >> 8) +
                    128;
                ++bins_[static_cast<std::size_t>(255 - cr) * kSize + cb];
                ++sample_count;
            }
        }
    }
    MapBinsToImage(sample_count);
}

std::optional<NormalizedPoint> Vectorscope::Project(const FloatColor& color) const {
    // Floating point throughout: markers need sub-bin positions.
    const ChromaCoefficients& matrix = CoefficientsFor(settings_.matrix);
    const float cb =
        (matrix.cb_from_r * color.r + matrix.cb_from_g * color.g + matrix.cb_from_b * color.b) /
            256.0f +
        128.0f;
    const float cr =
        (matrix.cr_from_r * color.r + matrix.cr_from_g * color.g + matrix.cr_from_b * color.b) /
            256.0f +
        128.0f;
    return NormalizedPoint{cb / 255.0f, (255.0f - cr) / 255.0f};
}

void Vectorscope::RebuildTintTable() {
    // Each bin is shown in the hue it represents: invert the chroma
    // transform at a fixed mid luma. BT.601 inverse coefficients are close
    // enough for display tinting in both matrix modes.
    constexpr float kDisplayLuma = 160.0f;
    for (int py = 0; py < kSize; ++py) {
        for (int px = 0; px < kSize; ++px) {
            const float cb = static_cast<float>(px) - 128.0f;
            const float cr = static_cast<float>(255 - py) - 128.0f;
            const auto to_byte = [](float value) {
                return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
            };
            uint8_t* tint = tint_.data() + (static_cast<std::size_t>(py) * kSize + px) * 3;
            tint[0] = to_byte(kDisplayLuma + 1.402f * cr);
            tint[1] = to_byte(kDisplayLuma - 0.344f * cb - 0.714f * cr);
            tint[2] = to_byte(kDisplayLuma + 1.772f * cb);
        }
    }
}

void Vectorscope::MapBinsToImage(uint64_t sample_count) {
    uint32_t densest = 0;
    for (const uint32_t count : bins_) densest = std::max(densest, count);

    const double per_sample_scale =
        sample_count > 0 ? kReferenceSampleCount / static_cast<double>(sample_count) : 0.0;
    const double gain = static_cast<double>(settings_.gain) * per_sample_scale;
    const double log_ceiling = densest > 0 ? std::log1p(static_cast<double>(densest) * gain) : 0.0;
    const double intensity_scale = log_ceiling > 0.0 ? 1.0 / log_ceiling : 0.0;

    uint8_t* out = image_.rgba.data();
    const uint8_t* tint = tint_.data();
    for (std::size_t i = 0; i < bins_.size(); ++i, out += 4, tint += 3) {
        const uint32_t count = bins_[i];
        if (count == 0) {
            out[0] = out[1] = out[2] = 0;
            out[3] = 255;
            continue;
        }
        const float brightness =
            static_cast<float>(std::log1p(static_cast<double>(count) * gain) * intensity_scale);
        out[0] = static_cast<uint8_t>(tint[0] * brightness);
        out[1] = static_cast<uint8_t>(tint[1] * brightness);
        out[2] = static_cast<uint8_t>(tint[2] * brightness);
        out[3] = 255;
    }
    ++image_.sequence;
}

}  // namespace sidescopes
