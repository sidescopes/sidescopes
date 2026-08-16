#pragma once

#include <cstddef>
#include <cstdint>

namespace sidescopes {

/// @brief The adjustments the lab's visitor can make to the picture.
///
/// A defined teaching pipeline, not an emulation of a source application's
/// color processing. Each control produces a reproducible change for observing
/// the resulting scope traces.
///
/// Every value is zero at rest, so a default-constructed set is the picture as
/// it was decoded.
struct ImageAdjustments
{
    /// Stops of exposure, applied in LINEAR light because that is what a stop
    /// means within this assumed-sRGB pipeline.
    float exposure = 0.0f;
    /// Encoded-value slope around code value 0.5. Positive steepens.
    float contrast = 0.0f;
    /// The top and the bottom of the range, separately, which is the pair that
    /// shows the waveform's two ends moving on their own.
    float highlights = 0.0f;
    float shadows = 0.0f;
    /// Cool to warm: blue down and red up as it rises.
    float temperature = 0.0f;
    /// Green to magenta.
    float tint = 0.0f;
    /// Toward the encoded Y' value at -1, away from it above 0.
    float saturation = 0.0f;

    /// Whether this leaves the picture exactly as it was. The caller can then
    /// skip the pass entirely - and, more importantly, a test can assert that
    /// a neutral set really is a no-op rather than a very small change.
    [[nodiscard]] bool neutral() const;

    [[nodiscard]] bool operator==(const ImageAdjustments&) const = default;
};

/// Writes @p pixels adjusted BGRA pixels into @p out, reading @p source.
///
/// ALWAYS FROM THE SOURCE, never in place over a previous result. Adjustments
/// that accumulate would introduce order- and interaction-dependent rounding
/// as a slider is dragged back and forth.
///
/// @p source and @p out may not overlap. Alpha is copied through untouched.
void applyAdjustments(const uint8_t* source, uint8_t* out, std::size_t pixels, const ImageAdjustments& adjustments);

}  // namespace sidescopes
