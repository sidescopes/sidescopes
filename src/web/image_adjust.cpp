// The demo's adjustments, as a per-channel tone curve plus one per-pixel step.
//
// Everything that is a function of a single channel's value - the white
// balance gain, exposure, the highlight and shadow rolls, and contrast -
// collapses into one 256-entry table per channel, built once when a control
// moves. Only saturation needs the other channels, because it is defined
// against the pixel's own luma. So a pass is three table lookups and a short
// blend per pixel, which holds a slider's drag on a photograph of a million
// pixels without dropping the frame loop.
//
// The maths is chosen to be RECOGNISABLE rather than to match any particular
// editor: a stop is a doubling in linear light, contrast pivots on mid grey,
// and warming raises red while lowering blue. A visitor who moves a control
// here and then moves the same-named control in Lightroom should see the
// scopes do the same KIND of thing, which is the whole purpose. Matching
// Lightroom's actual curves is neither possible nor the point.

#include "web/image_adjust.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sidescopes {
namespace {

/// The sRGB transfer function and its inverse. Exposure and white balance are
/// light, so they happen between these two.
[[nodiscard]] float toLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] float toGamma(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

/// Rec. 709 luma, the same weighting the luma waveform uses, so a visitor
/// pulling saturation to nothing sees the vectorscope collapse onto a point
/// that agrees with the scope beside it.
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

/// The channel gains a temperature and tint pair asks for, in linear light.
/// Warm raises red and lowers blue; magenta raises red and blue together
/// while lowering green. Kept small - a quarter either way at the extremes -
/// because the lesson is that a cast MOVES THE WHOLE CLOUD, and a violent one
/// just clips.
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

        // Then the two ends, each acting where the other does not. BOTH add:
        // positive lifts, negative recovers, which is the direction every
        // editor a photographer already uses gives these two. Having shadows
        // subtract would be defensible arithmetic and the wrong control.
        value += adjustments.highlights * 0.35f * highlightWeight(value);
        value += adjustments.shadows * 0.35f * shadowWeight(value);

        // Contrast last, pivoting on mid grey so it opens and closes the
        // range around the middle rather than dragging it up or down.
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
        // Byte for byte, and not merely very close: a visitor who returns
        // every control to zero is entitled to the photograph they started
        // with, and a scope that did not settle back exactly would be
        // reporting on the arithmetic rather than on the picture.
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
