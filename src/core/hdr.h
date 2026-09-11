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

/// ScreenCaptureKit Display P3 PQ half-float RGB to clipped sRGB ten-bit
/// codes plus unclipped linear luminance relative to its 100-nit encoding
/// reference. Tables avoid transfer-function powers in the frame loop.
class PqCaptureDecoder
{
public:
    PqCaptureDecoder();
    void convertRow(const uint8_t* rgbaHalf, uint8_t* argb2101010, float* luminance, int width) const;

private:
    [[nodiscard]] uint16_t displayCode(float linear) const;
    std::vector<float> m_linear;
    std::vector<uint16_t> m_codes;
};

}  // namespace sidescopes
