#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sidescopes {

/// The luminance one unit of scRGB stands for. Windows composes an advanced
/// color desktop (HDR, or Auto Color Management) in linear scRGB, where 1.0 is
/// 80 nits, and states the user's SDR white level as a multiple of that value.
inline constexpr double ScrgbWhiteNits = 80.0;

/// The sRGB transfer function: a linear value in 0..1 to its display-encoded
/// value in 0..1. The inverse of the decode the scopes evaluate per code.
[[nodiscard]] double encodedFromLinear(double linear);

/// An IEEE half-precision pattern as a float, exact for every finite pattern.
/// Infinities and NaNs come through as such.
[[nodiscard]] float floatFromHalf(uint16_t half);

/// Converts scRGB half-float pixels - what desktop duplication delivers while
/// the desktop composes in HDR or with Auto Color Management - into the
/// Argb2101010 layout the scopes already read.
///
/// Each channel is divided by the SDR white level, so SDR content produces the
/// codes it produces when the desktop composes in 8-bit SDR; clamped to 0..1,
/// so content brighter than SDR white saturates at full scale and negative or
/// out-of-gamut excursions read as black; and encoded with the sRGB transfer
/// function on the 0..1023 scale. Every half pattern has its code computed
/// once per white level, so a frame costs three table reads per pixel.
class ScrgbToDisplayCodes
{
public:
    explicit ScrgbToDisplayCodes(double sdrWhiteNits = ScrgbWhiteNits);

    /// Rebuilds the code table when @p nits differs from the current level.
    /// Non-positive values fall back to the scRGB white.
    void setSdrWhiteNits(double nits);

    [[nodiscard]] double sdrWhiteNits() const
    {
        return m_sdrWhiteNits;
    }

    /// The 10-bit display code for one half-precision channel value.
    [[nodiscard]] uint16_t codeFor(uint16_t half) const
    {
        return m_codes[half];
    }

    /// Converts @p width pixels of one R16G16B16A16_FLOAT row - eight bytes per
    /// pixel, little-endian halves in red, green, blue, alpha order - into
    /// @p width Argb2101010 pixels of four bytes each, written opaque.
    void convertRow(const uint8_t* scrgbPixels, uint8_t* argb2101010Pixels, int width) const;

private:
    double m_sdrWhiteNits;
    std::vector<uint16_t> m_codes;
};

}  // namespace sidescopes
