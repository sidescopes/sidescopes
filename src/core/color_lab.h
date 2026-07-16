#pragma once

#include "core/frame.h"

namespace sidescopes {

/// CIELAB coordinates under a D65 white point.
struct LabColor
{
    float lightness = 0.0f;  ///< L*, 0-100.
    float a = 0.0f;          ///< green(-) to magenta(+).
    float b = 0.0f;          ///< blue(-) to yellow(+).
};

/// A colour difference decomposed the way a colourist reads it, with the
/// perceptual magnitude alongside.
struct ColorDifference
{
    float lightness = 0.0f;  ///< dL*, signed.
    float chroma = 0.0f;     ///< dC*, signed.
    float hue = 0.0f;        ///< dH*, chroma-weighted (self-gating near neutral).
    float deltaE = 0.0f;     ///< CIEDE2000 magnitude.
};

/// Converts a display sRGB colour (0-255 per channel) to CIELAB under D65.
/// Channels outside the range are clamped into it.
[[nodiscard]] LabColor labFromSrgb(const FloatColor& srgb);

/// @return The chroma C* of @p lab.
[[nodiscard]] float chromaOf(const LabColor& lab);

/// The difference from @p reference to @p sample.
[[nodiscard]] ColorDifference differenceFrom(const LabColor& reference, const LabColor& sample);

/// CIEDE2000 colour difference between two CIELAB colours, with every
/// parametric weighting factor at 1.
[[nodiscard]] float deltaE2000(const LabColor& first, const LabColor& second);

}  // namespace sidescopes
