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
    settings_.size = std::clamp(settings_.size, kSize, 512);
    if (settings_.size != size_)
        Resize(settings_.size);
    else if (matrix_changed)
        RebuildTintTable();
}

void Vectorscope::Resize(int size) {
    size_ = size;
    bins_.assign(static_cast<std::size_t>(kSize) * kSize, 0);
    smoothed_.assign(static_cast<std::size_t>(kSize) * kSize, 0.0f);
    upsampled_.assign(size_ > kSize ? static_cast<std::size_t>(size_) * size_ : 0, 0.0f);
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
            // Accumulation always uses the 256-code grid: 8-bit content
            // quantizes its chroma to those codes (and piles unevenly
            // onto fixed sub-code positions, measured at 45% of a grass
            // frame on a single one), so a finer accumulation grid
            // renders quantization as gridded texture. Samples splat
            // bilinearly across the four bins they straddle, in
            // sixteenths per axis: truncation used to alias the code
            // lattice into texture even at 256, and parked the whole
            // cloud half a bin off the positions the projection - and
            // with it every marker and graticule target - reports.
            const int size = kSize;
            const int span = size * 16;
            for (; pixel < row_end; pixel += static_cast<std::ptrdiff_t>(4) * stride) {
                const int b = pixel[0], g = pixel[1], r = pixel[2];
                const int64_t cb_raw =
                    matrix.cb_from_r * r + matrix.cb_from_g * g + matrix.cb_from_b * b;
                const int64_t cr_raw =
                    matrix.cr_from_r * r + matrix.cr_from_g * g + matrix.cr_from_b * b;
                const int cb_position =
                    std::clamp(static_cast<int>(cb_raw * span >> 16) + span / 2, 0, span - 17);
                const int cr_position =
                    std::clamp(static_cast<int>(cr_raw * span >> 16) + span / 2, 0, span - 17);
                const int cb_bin = cb_position >> 4;
                const int cr_bin = cr_position >> 4;
                const uint32_t cb_high = static_cast<uint32_t>(cb_position & 15);
                const uint32_t cr_high = static_cast<uint32_t>(cr_position & 15);
                const uint32_t cb_low = 16 - cb_high;
                const uint32_t cr_low = 16 - cr_high;
                uint32_t* upper =
                    bins_.data() + static_cast<std::size_t>(size - 1 - cr_bin) * size + cb_bin;
                uint32_t* above = upper - size;  // cr_bin + 1 is one row up
                upper[0] += cb_low * cr_low;
                upper[1] += cb_high * cr_low;
                above[0] += cb_low * cr_high;
                above[1] += cb_high * cr_high;
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
    // A half-strength 3x3 binomial takes the worst speckle out of the
    // cloud without softening it: the fractional splat already spreads
    // each sample as a tent, so a full binomial on top reads as gaussian
    // blur at large pane sizes.
    for (int py = 0; py < kSize; ++py) {
        for (int px = 0; px < kSize; ++px) {
            const auto at = [&](int y, int x) -> float {
                if (y < 0 || y >= kSize || x < 0 || x >= kSize) return 0.0f;
                return static_cast<float>(bins_[static_cast<std::size_t>(y) * kSize + x]);
            };
            const auto row = [&](int y) {
                return at(y, px - 1) + 2.0f * at(y, px) + at(y, px + 1);
            };
            smoothed_[static_cast<std::size_t>(py) * kSize + px] =
                0.5f * at(py, px) + 0.5f * (row(py - 1) + 2.0f * row(py) + row(py + 1)) / 16.0f;
        }
    }

    // A finer display image interpolates the code grid with a separable
    // Catmull-Rom, the same reconstruction the waveform uses for its
    // level axis: the cloud gets the large pane's smoothness, and peak
    // positions keep the sub-code placement the fractional splat gave
    // them, without pretending to resolve chroma below one code.
    const float* densities = smoothed_.data();
    int density_size = kSize;
    if (size_ > kSize) {
        const auto at = [&](int y, int x) -> float {
            y = std::clamp(y, 0, kSize - 1);
            x = std::clamp(x, 0, kSize - 1);
            return smoothed_[static_cast<std::size_t>(y) * kSize + x];
        };
        const auto weights = [](float t, float w[4]) {
            w[0] = ((-0.5f * t + 1.0f) * t - 0.5f) * t;
            w[1] = (1.5f * t - 2.5f) * t * t + 1.0f;
            w[2] = ((-1.5f * t + 2.0f) * t + 0.5f) * t;
            w[3] = (0.5f * t - 0.5f) * t * t;
        };
        const float step = static_cast<float>(kSize) / static_cast<float>(size_);
        for (int py = 0; py < size_; ++py) {
            const float sy = (static_cast<float>(py) + 0.5f) * step - 0.5f;
            const int base_y = static_cast<int>(std::floor(sy));
            float wy[4];
            weights(sy - static_cast<float>(base_y), wy);
            for (int px = 0; px < size_; ++px) {
                const float sx = (static_cast<float>(px) + 0.5f) * step - 0.5f;
                const int base_x = static_cast<int>(std::floor(sx));
                float wx[4];
                weights(sx - static_cast<float>(base_x), wx);
                float value = 0.0f;
                for (int j = 0; j < 4; ++j) {
                    float row_value = 0.0f;
                    for (int i = 0; i < 4; ++i)
                        row_value += wx[i] * at(base_y - 1 + j, base_x - 1 + i);
                    value += wy[j] * row_value;
                }
                // Catmull-Rom undershoots next to sharp peaks; densities
                // cannot be negative.
                upsampled_[static_cast<std::size_t>(py) * size_ + px] = std::max(value, 0.0f);
            }
        }
        densities = upsampled_.data();
        density_size = size_;
    }

    float densest = 0.0f;
    const std::size_t density_count = static_cast<std::size_t>(density_size) * density_size;
    for (std::size_t i = 0; i < density_count; ++i) densest = std::max(densest, densities[i]);

    // Each sample contributes 256 weight units (the splat's sixteenths
    // squared); the normalization divides them back out so the gain
    // keeps its calibrated feel.
    const double per_sample_scale =
        sample_count > 0 ? kReferenceSampleCount / (static_cast<double>(sample_count) * 256.0)
                         : 0.0;
    const double gain = static_cast<double>(settings_.gain) * per_sample_scale;
    const double log_ceiling = densest > 0.0f ? std::log1p(densest * gain) : 0.0;
    const double intensity_scale = log_ceiling > 0.0 ? 1.0 / log_ceiling : 0.0;

    uint8_t* out = image_.rgba.data();
    const uint8_t* tint = tint_.data();
    for (std::size_t i = 0; i < density_count; ++i, out += 4, tint += 3) {
        const float count = densities[i];
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
