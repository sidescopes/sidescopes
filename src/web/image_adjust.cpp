// The lab's adjustments, as a per-channel tone curve plus one per-pixel step.
//
// Everything that is a function of a single channel's value - the white
// balance gain, exposure, the highlight and shadow rolls, and contrast -
// collapses into one 256-entry table per channel, built once when a control
// moves. Only saturation needs the other channels, because it is defined
// against the pixel's own luma. So a pass is three table lookups and a short
// blend per pixel, which holds a slider's drag on a photograph of a million
// pixels without dropping the frame loop.
//
// This pipeline is intentionally small and defined rather than an imitation
// of a particular editor. Exposure is a linear-light multiplier, the balance
// controls are simple linear RGB gains, the tonal curves operate on the
// encoded result, and saturation is an encoded-RGB displacement from Y'. The
// same-named controls in another application can use different spaces,
// pivots, tone curves, or appearance models and need not produce the same
// values.

#include "web/image_adjust.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sidescopes {
namespace {

/// The sRGB transfer function and its inverse. Exposure and the Lab's simple
/// channel-balance gains are applied to linearized values between these two.
[[nodiscard]] float toLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] float toGamma(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

/// Rec.709-style encoded Y', using the same coefficients as the luma waveform.
[[nodiscard]] float luma(float red, float green, float blue)
{
    return 0.2126f * red + 0.7152f * green + 0.0722f * blue;
}

/// How strongly the highlight and shadow rolls act at a given value: each
/// fades out well before the other's end, so raising shadows leaves the
/// highlights where they were - which is the point of having both.
[[nodiscard]] float highlightWeight(float value)
{
    return std::clamp((value - 0.5f) * 2.0f, 0.0f, 1.0f);
}

[[nodiscard]] float shadowWeight(float value)
{
    return std::clamp((0.5f - value) * 2.0f, 0.0f, 1.0f);
}

/// The channel gains used by the directional Temperature and Tint controls.
/// These are not a correlated-color-temperature model or a chromatic
/// adaptation transform.
struct ChannelGain
{
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
};

[[nodiscard]] ChannelGain balanceGain(float temperature, float tint)
{
    ChannelGain gain;
    gain.red = 1.0f + temperature * 0.25f + tint * 0.12f;
    gain.green = 1.0f - tint * 0.20f;
    gain.blue = 1.0f - temperature * 0.25f + tint * 0.12f;

    return gain;
}

/// One channel's table: every byte value put through white balance, exposure,
/// the two rolls and contrast, in that order.
using Curve = std::array<uint8_t, 256>;

[[nodiscard]] Curve buildCurve(const ImageAdjustments& adjustments, float gain)
{
    const float scale = std::pow(2.0f, adjustments.exposure) * gain;
    Curve curve{};
    for (int step = 0; step < 256; ++step) {
        const float encoded = static_cast<float>(step) / 255.0f;

        // Light first: a stop is a doubling, and a cast is a gain on light.
        float value = toGamma(std::max(0.0f, toLinear(encoded) * scale));

        // The two fixed encoded-value rolls meet at the pivot. Positive values
        // raise their part of the range and negative values lower it.
        value += adjustments.highlights * 0.35f * highlightWeight(value);
        value += adjustments.shadows * 0.35f * shadowWeight(value);

        // Contrast is a linear encoded-value slope around code value 0.5.
        value = 0.5f + (value - 0.5f) * (1.0f + adjustments.contrast);

        curve[static_cast<std::size_t>(step)] =
            static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    return curve;
}

}  // namespace

bool ImageAdjustments::neutral() const
{
    return *this == ImageAdjustments{};
}

void applyAdjustments(const uint8_t* source, uint8_t* out, std::size_t pixels, const ImageAdjustments& adjustments)
{
    if (source == nullptr || out == nullptr) {
        return;
    }
    if (adjustments.neutral()) {
        // Preserve the decoded input byte for byte when every control is at
        // zero, rather than round-tripping it through the transfer functions.
        std::copy_n(source, pixels * 4u, out);

        return;
    }

    const ChannelGain gain = balanceGain(adjustments.temperature, adjustments.tint);
    const Curve blueCurve = buildCurve(adjustments, gain.blue);
    const Curve greenCurve = buildCurve(adjustments, gain.green);
    const Curve redCurve = buildCurve(adjustments, gain.red);
    const float saturation = 1.0f + adjustments.saturation;

    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const std::size_t at = pixel * 4u;
        float blue = static_cast<float>(blueCurve[source[at]]) / 255.0f;
        float green = static_cast<float>(greenCurve[source[at + 1]]) / 255.0f;
        float red = static_cast<float>(redCurve[source[at + 2]]) / 255.0f;

        if (adjustments.saturation != 0.0f) {
            const float grey = luma(red, green, blue);
            red = grey + (red - grey) * saturation;
            green = grey + (green - grey) * saturation;
            blue = grey + (blue - grey) * saturation;
        }

        out[at] = static_cast<uint8_t>(std::lround(std::clamp(blue, 0.0f, 1.0f) * 255.0f));
        out[at + 1] = static_cast<uint8_t>(std::lround(std::clamp(green, 0.0f, 1.0f) * 255.0f));
        out[at + 2] = static_cast<uint8_t>(std::lround(std::clamp(red, 0.0f, 1.0f) * 255.0f));
        out[at + 3] = source[at + 3];
    }
}

}  // namespace sidescopes
