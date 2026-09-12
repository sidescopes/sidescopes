#pragma once

#include <cstdint>
#include <vector>

namespace sidescopes {

/// SMPTE ST 2084 EOTF. Returns nominal nits; invalid codes return NaN.
[[nodiscard]] double pqNits(double encoded);

/// Linear-light Rec.709 luminance. Signed components retain out-of-gamut
/// contributions; only the final luminance is bounded at black. Non-finite
/// input produces an invalid (NaN) measurement.
[[nodiscard]] float hdrLuminance709(float red, float green, float blue);

/// The sRGB transfer function continued above 1.0 - the curve the display
/// codes are encoded with, so a colour above SDR white reads on the scale the
/// readout already speaks: twice white encodes to about 1.35. Nothing at or
/// below zero encodes above zero.
[[nodiscard]] double extendedSrgbFromLinear(double linear);

/// ScreenCaptureKit Display P3 PQ half-float RGB to clipped sRGB ten-bit
/// codes plus unclipped linear luminance and colour relative to its 100-nit
/// encoding reference. Tables avoid transfer-function powers in the frame loop.
class PqCaptureDecoder
{
public:
    PqCaptureDecoder();
    /// @p linear, when given, receives three halves per pixel: the unclipped
    /// linear-light sRGB colour, signed where Display P3 reaches outside sRGB.
    void convertRow(const uint8_t* rgbaHalf, uint8_t* argb2101010, float* luminance, int width,
                    uint16_t* linear = nullptr) const;
    /// The unclipped linear sRGB colour of one PQ pixel given as halves.
    void linearRgb(uint16_t redHalf, uint16_t greenHalf, uint16_t blueHalf, float& red, float& green,
                   float& blue) const;

private:
    [[nodiscard]] uint16_t displayCode(float linear) const;
    std::vector<float> m_linear;
    std::vector<uint16_t> m_codes;
};

}  // namespace sidescopes
