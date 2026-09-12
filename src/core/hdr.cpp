#include "core/hdr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/scrgb.h"

namespace sidescopes {

double pqNits(double encoded)
{
    if (!std::isfinite(encoded) || encoded < 0.0 || encoded > 1.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    constexpr double M1 = 2610.0 / 16384.0;
    constexpr double M2 = 2523.0 / 32.0;
    constexpr double C1 = 3424.0 / 4096.0;
    constexpr double C2 = 2413.0 / 128.0;
    constexpr double C3 = 2392.0 / 128.0;
    const double power = std::pow(encoded, 1.0 / M2);
    return 10000.0 * std::pow(std::max(power - C1, 0.0) / (C2 - C3 * power), 1.0 / M1);
}

double extendedSrgbFromLinear(double linear)
{
    return linear > 0.0 ? encodedFromLinear(linear) : 0.0;
}

float hdrLuminance709(float red, float green, float blue)
{
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return std::max(0.0f, 0.212639f * red + 0.715169f * green + 0.072192f * blue);
}

PqCaptureDecoder::PqCaptureDecoder()
    : m_linear(65536),
      m_codes(65536)
{
    for (std::size_t index = 0; index < m_linear.size(); ++index) {
        m_linear[index] = static_cast<float>(pqNits(floatFromHalf(static_cast<uint16_t>(index))) / 100.0);
        m_codes[index] =
            static_cast<uint16_t>(std::lround(encodedFromLinear(static_cast<double>(index) / 65535.0) * 1023.0));
    }
}

uint16_t PqCaptureDecoder::displayCode(float linear) const
{
    if (!(linear > 0.0f)) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(std::lround(std::min(linear, 1.0f) * 65535.0f));
    return m_codes[index];
}

void PqCaptureDecoder::linearRgb(uint16_t redHalf, uint16_t greenHalf, uint16_t blueHalf, float& red, float& green,
                                 float& blue) const
{
    const float r = m_linear[redHalf];
    const float g = m_linear[greenHalf];
    const float b = m_linear[blueHalf];
    // D65 Display P3 -> D65 sRGB in linear light. Never clamp before
    // measuring luminance: saturated colors can have signed components.
    red = 1.22494018f * r - 0.22494018f * g;
    green = -0.04205695f * r + 1.04205695f * g;
    blue = -0.01963755f * r - 0.07863605f * g + 1.09827360f * b;
}

void PqCaptureDecoder::convertRow(const uint8_t* rgbaHalf, uint8_t* argb2101010, float* luminance, int width,
                                  uint16_t* linear) const
{
    for (int x = 0; x < width; ++x) {
        const uint8_t* source = rgbaHalf + static_cast<std::size_t>(x) * 8;
        const auto half = [source](int index) {
            const int offset = index * 2;
            return static_cast<uint16_t>(source[offset] | source[offset + 1] << 8);
        };
        float sr = 0.0f;
        float sg = 0.0f;
        float sb = 0.0f;
        linearRgb(half(0), half(1), half(2), sr, sg, sb);
        luminance[x] = hdrLuminance709(sr, sg, sb);
        if (linear != nullptr) {
            uint16_t* colour = linear + static_cast<std::size_t>(x) * 3;
            colour[0] = halfFromFloat(sr);
            colour[1] = halfFromFloat(sg);
            colour[2] = halfFromFloat(sb);
        }
        const uint32_t word = 0xC0000000u | static_cast<uint32_t>(displayCode(sr)) << 20 |
                              static_cast<uint32_t>(displayCode(sg)) << 10 | displayCode(sb);
        uint8_t* target = argb2101010 + static_cast<std::size_t>(x) * 4;
        target[0] = static_cast<uint8_t>(word);
        target[1] = static_cast<uint8_t>(word >> 8);
        target[2] = static_cast<uint8_t>(word >> 16);
        target[3] = static_cast<uint8_t>(word >> 24);
    }
}

}  // namespace sidescopes
