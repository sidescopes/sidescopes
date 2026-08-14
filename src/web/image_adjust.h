#pragma once

#include <cstddef>
#include <cstdint>

namespace sidescopes {

/// @brief The adjustments the demo's visitor can make to the picture.
///
/// Not an editor, and deliberately not trying to be one: this exists so that
/// moving a control and watching a scope answer teaches what the scope
/// MEASURES. Each one owns a scope - exposure and contrast move the waveform
/// and the histogram, the white balance pair moves the vectorscope's cloud off
/// centre, saturation moves it in and out along the radius, and highlights and
/// shadows move the two ends of the waveform independently.
///
/// Every value is zero at rest, so a default-constructed set is the picture as
/// it was decoded.
struct ImageAdjustments
{
    /// Stops of exposure, applied in LINEAR light because that is what a stop
    /// means. Doing it in gamma space would clip highlights in a way no
    /// photographer would recognise.
    float exposure = 0.0f;
    /// Around mid grey. Positive steepens.
    float contrast = 0.0f;
    /// The top and the bottom of the range, separately, which is the pair that
    /// shows the waveform's two ends moving on their own.
    float highlights = 0.0f;
    float shadows = 0.0f;
    /// Cool to warm: blue down and red up as it rises.
    float temperature = 0.0f;
    /// Green to magenta.
    float tint = 0.0f;
    /// Toward grey at -1, away from it above 0.
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
/// that accumulate would degrade the picture as a slider is dragged back and
/// forth, and the scopes would then show that degradation as though it were
/// something about the photograph.
///
/// @p source and @p out may not overlap. Alpha is copied through untouched.
void applyAdjustments(const uint8_t* source, uint8_t* out, std::size_t pixels, const ImageAdjustments& adjustments);

}  // namespace sidescopes
