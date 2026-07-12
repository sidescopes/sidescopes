#include "core/scopes/vectorscope.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

Vectorscope::Vectorscope() {
    Resize(kDefaultVectorscopeSize);
}

void Vectorscope::Configure(const VectorscopeSettings& settings) {
    const bool matrix_changed = settings.matrix != settings_.matrix;
    settings_ = settings;
    settings_.sampling_stride = std::clamp(settings_.sampling_stride, 1, 8);
    settings_.size = std::clamp(settings_.size, 128, 512);
    if (settings_.size != size_)
        Resize(settings_.size);
    else if (matrix_changed)
        RebuildTintTable();
}

void Vectorscope::Resize(int size) {
    size_ = size;
    bins_.assign(static_cast<std::size_t>(size_) * size_, 0);
    tint_.assign(static_cast<std::size_t>(size_) * size_ * 3, 0);
    image_.width = size_;
    image_.height = size_;
    image_.rgba.assign(static_cast<std::size_t>(size_) * size_ * 4, 0);
    RebuildTintTable();
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
            // The raw transform carries eight bits below the classic 256
            // grid; scaling it to the configured grid instead of always
            // shifting by eight is what makes a finer grid real detail.
            const int size = size_;
            for (; pixel < row_end; pixel += static_cast<std::ptrdiff_t>(4) * stride) {
                const int b = pixel[0], g = pixel[1], r = pixel[2];
                const int cb_raw =
                    matrix.cb_from_r * r + matrix.cb_from_g * g + matrix.cb_from_b * b;
                const int cr_raw =
                    matrix.cr_from_r * r + matrix.cr_from_g * g + matrix.cr_from_b * b;
                const int cb = std::clamp((cb_raw * size >> 16) + size / 2, 0, size - 1);
                const int cr = std::clamp((cr_raw * size >> 16) + size / 2, 0, size - 1);
                ++bins_[static_cast<std::size_t>(size - 1 - cr) * size + cb];
                ++sample_count;
            }
        }
    }
    MapBinsToImage(sample_count);
}

std::optional<NormalizedPoint> Vectorscope::Project(const FloatColor& color) const {
    // Floating point throughout: markers need sub-bin positions.
    const ChromaCoefficients& matrix = CoefficientsFor(settings_.matrix);
    const float cb = (static_cast<float>(matrix.cb_from_r) * color.r +
                      static_cast<float>(matrix.cb_from_g) * color.g +
                      static_cast<float>(matrix.cb_from_b) * color.b) /
                         256.0f +
                     128.0f;
    const float cr = (static_cast<float>(matrix.cr_from_r) * color.r +
                      static_cast<float>(matrix.cr_from_g) * color.g +
                      static_cast<float>(matrix.cr_from_b) * color.b) /
                         256.0f +
                     128.0f;
    return NormalizedPoint{cb / 255.0f, (255.0f - cr) / 255.0f};
}

void Vectorscope::RebuildTintTable() {
    // Each bin is shown in the hue it represents: invert the chroma
    // transform at a fixed mid luma. BT.601 inverse coefficients are close
    // enough for display tinting in both matrix modes.
    constexpr float kDisplayLuma = 160.0f;
    // Chroma is exaggerated for display: the trace paints the hue a bin
    // represents, not a colorimetric reproduction, and at true saturation
    // the cloud reads as washed-out pastel.
    constexpr float kSaturationBoost = 1.7f;
    const float to_chroma = 256.0f / static_cast<float>(size_);
    for (int py = 0; py < size_; ++py) {
        for (int px = 0; px < size_; ++px) {
            const float cb = (static_cast<float>(px) * to_chroma - 128.0f) * kSaturationBoost;
            const float cr =
                (static_cast<float>(size_ - 1 - py) * to_chroma - 128.0f) * kSaturationBoost;
            const auto to_byte = [](float value) {
                return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
            };
            uint8_t* tint = tint_.data() + (static_cast<std::size_t>(py) * size_ + px) * 3;
            tint[0] = to_byte(kDisplayLuma + 1.402f * cr);
            tint[1] = to_byte(kDisplayLuma - 0.344f * cb - 0.714f * cr);
            tint[2] = to_byte(kDisplayLuma + 1.772f * cb);
        }
    }
}

void Vectorscope::MapBinsToImage(uint64_t sample_count) {
    // A 3x3 binomial blur takes the speckle out of the cloud: chroma
    // coordinates quantize to integers, and raw bins render as grainy
    // single-pixel noise where the photograph's colors actually vary
    // smoothly.
    std::vector<float> smoothed(bins_.size(), 0.0f);
    for (int py = 0; py < size_; ++py) {
        for (int px = 0; px < size_; ++px) {
            const auto at = [&](int y, int x) -> float {
                if (y < 0 || y >= size_ || x < 0 || x >= size_) return 0.0f;
                return static_cast<float>(bins_[static_cast<std::size_t>(y) * size_ + x]);
            };
            const auto row = [&](int y) {
                return at(y, px - 1) + 2.0f * at(y, px) + at(y, px + 1);
            };
            smoothed[static_cast<std::size_t>(py) * size_ + px] =
                (row(py - 1) + 2.0f * row(py) + row(py + 1)) / 16.0f;
        }
    }

    float densest = 0.0f;
    for (const float count : smoothed) densest = std::max(densest, count);

    const double per_sample_scale =
        sample_count > 0 ? kReferenceSampleCount / static_cast<double>(sample_count) : 0.0;
    const double gain = static_cast<double>(settings_.gain) * per_sample_scale;
    const double log_ceiling = densest > 0.0f ? std::log1p(densest * gain) : 0.0;
    const double intensity_scale = log_ceiling > 0.0 ? 1.0 / log_ceiling : 0.0;

    uint8_t* out = image_.rgba.data();
    const uint8_t* tint = tint_.data();
    for (std::size_t i = 0; i < smoothed.size(); ++i, out += 4, tint += 3) {
        const float count = smoothed[i];
        if (count <= 0.0f) {
            out[0] = out[1] = out[2] = 0;
            out[3] = 255;
            continue;
        }
        // The gamma lifts the mid-density body of the cloud: normalizing
        // to the densest bin pushes everything else down, and a linear
        // ramp leaves the trace dim at any gain.
        const float normalized =
            static_cast<float>(std::log1p(static_cast<double>(count) * gain) * intensity_scale);
        const float brightness = std::pow(normalized, 0.65f);
        out[0] = static_cast<uint8_t>(static_cast<float>(tint[0]) * brightness);
        out[1] = static_cast<uint8_t>(static_cast<float>(tint[1]) * brightness);
        out[2] = static_cast<uint8_t>(static_cast<float>(tint[2]) * brightness);
        out[3] = 255;
    }
    ++image_.sequence;
}

}  // namespace sidescopes
