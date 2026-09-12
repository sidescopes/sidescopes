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

uint16_t halfFromFloat(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t floatExponent = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;
    if (floatExponent == 0xFFu) {
        return static_cast<uint16_t>(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u));
    }
    const int exponent = static_cast<int>(floatExponent) - 127 + 15;
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (exponent <= 0) {
        // A half subnormal, or nothing: the implicit bit joins the mantissa and
        // the whole is shifted to the fixed 2^-24 step, rounded to even.
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const int shift = 14 - exponent;
        uint32_t half = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half & 1u) != 0)) {
            ++half;
        }

        return static_cast<uint16_t>(sign | half);
    }
    // Rounding may carry into the exponent, which is the right answer there
    // too: the value just below a power of two rounds up to it, and the value
    // just below the range rounds up to infinity.
    uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u) != 0)) {
        ++half;
    }

    return static_cast<uint16_t>(sign | half);
}

int ScrgbToDisplayCodes::convertRow(const uint8_t* scrgbPixels, uint8_t* argb2101010Pixels, int width,
                                    float* hdrLuminance, uint16_t* hdrLinear) const
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
        if (hdrLuminance != nullptr || hdrLinear != nullptr) {
            const float scale = static_cast<float>(ScrgbWhiteNits / m_sdrWhiteNits);
            const float r = floatFromHalf(red) * scale;
            const float g = floatFromHalf(green) * scale;
            const float b = floatFromHalf(blue) * scale;
            if (hdrLuminance != nullptr) {
                hdrLuminance[x] = hdrLuminance709(r, g, b);
            }
            if (hdrLinear != nullptr) {
                uint16_t* linear = hdrLinear + static_cast<std::size_t>(x) * 3;
                linear[0] = halfFromFloat(r);
                linear[1] = halfFromFloat(g);
                linear[2] = halfFromFloat(b);
            }
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
