#include "core/scrgb.h"

#include <cmath>
#include <cstring>

#include "core/hdr.h"

namespace sidescopes {
namespace {

constexpr std::size_t HalfPatterns = 65536;
constexpr int MaxCode = 1023;

}  // namespace

double encodedFromLinear(double linear)
{
    if (linear <= 0.0031308) {
        return linear * 12.92;
    }

    return 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

float floatFromHalf(uint16_t half)
{
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa != 0) {
            // Subnormal: renormalize into a float exponent.
            int shift = 0;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x3FFu;
            bits = sign | (static_cast<uint32_t>(127 - 15 + 1 - shift) << 23) | (mantissa << 13);
        } else {
            bits = sign;
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);

    return value;
}

ScrgbToDisplayCodes::ScrgbToDisplayCodes(double sdrWhiteNits)
    : m_sdrWhiteNits(0.0),
      m_codes(HalfPatterns),
      m_aboveWhite(HalfPatterns)
{
    setSdrWhiteNits(sdrWhiteNits);
}

void ScrgbToDisplayCodes::setSdrWhiteNits(double nits)
{
    const double white = nits > 0.0 ? nits : ScrgbWhiteNits;
    if (white == m_sdrWhiteNits) {
        return;
    }
    m_sdrWhiteNits = white;
    const double scale = ScrgbWhiteNits / white;
    for (std::size_t pattern = 0; pattern < HalfPatterns; ++pattern) {
        const double linear = static_cast<double>(floatFromHalf(static_cast<uint16_t>(pattern))) * scale;
        uint16_t code = 0;
        // NaN fails both comparisons and reads as black, like every negative value.
        if (linear >= 1.0) {
            code = MaxCode;
        } else if (linear > 0.0) {
            code = static_cast<uint16_t>(std::lround(encodedFromLinear(linear) * MaxCode));
        }
        m_codes[pattern] = code;
        m_aboveWhite[pattern] = linear > 1.0 ? 1 : 0;
    }
}

int ScrgbToDisplayCodes::convertRow(const uint8_t* scrgbPixels, uint8_t* argb2101010Pixels, int width,
                                    float* hdrLuminance) const
{
    int above = 0;
    for (int x = 0; x < width; ++x) {
        const uint8_t* source = scrgbPixels + static_cast<std::size_t>(x) * 8;
        const auto half = [source](int channel) {
            const std::size_t offset = static_cast<std::size_t>(channel) * 2;
            return static_cast<uint16_t>(source[offset] | source[offset + 1] << 8);
        };
        const uint16_t red = half(0);
        const uint16_t green = half(1);
        const uint16_t blue = half(2);
        if (hdrLuminance) {
            const float scale = static_cast<float>(ScrgbWhiteNits / m_sdrWhiteNits);
            hdrLuminance[x] =
                hdrLuminance709(floatFromHalf(red) * scale, floatFromHalf(green) * scale, floatFromHalf(blue) * scale);
        }
        const uint32_t word = 0xC0000000u | static_cast<uint32_t>(codeFor(red)) << 20 |
                              static_cast<uint32_t>(codeFor(green)) << 10 | codeFor(blue);
        uint8_t* target = argb2101010Pixels + static_cast<std::size_t>(x) * 4;
        target[0] = static_cast<uint8_t>(word);
        target[1] = static_cast<uint8_t>(word >> 8);
        target[2] = static_cast<uint8_t>(word >> 16);
        target[3] = static_cast<uint8_t>(word >> 24);
        above += (aboveWhite(red) || aboveWhite(green) || aboveWhite(blue)) ? 1 : 0;
    }

    return above;
}

}  // namespace sidescopes
