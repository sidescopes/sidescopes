// The scRGB half-float to display-code conversion behind advanced color
// capture on Windows: the half decode, the sRGB encode, the white-level
// scaling, and the packed row the scopes read. The law it pins was measured on
// a Windows 11 desktop composing in HDR and with Auto Color Management: SDR
// content arrives as linear scRGB multiplied by the SDR white level, and
// dividing by that level and encoding gives back the source's codes exactly.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/frame.h"
#include "core/scrgb.h"

using namespace sidescopes;

namespace {

// IEEE half encoding of a float, round to nearest even, for finite inputs that
// stay within the half range. Independent of the decode under test.
uint16_t halfFromFloat(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        const uint32_t full = mantissa | 0x800000u;
        const int shift = 14 - exponent;
        uint32_t half = full >> shift;
        const uint32_t remainder = full & ((1u << shift) - 1);
        const uint32_t midpoint = 1u << (shift - 1);
        if (remainder > midpoint || (remainder == midpoint && (half & 1u))) {
            ++half;
        }
        return static_cast<uint16_t>(sign | half);
    }
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) {
        ++half;
    }

    return static_cast<uint16_t>(half);
}

double linearFromEncoded(double encoded)
{
    if (encoded <= 0.04045) {
        return encoded / 12.92;
    }

    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

// The reference the table is checked against: the same law written without a
// table, in double precision.
uint16_t referenceCode(uint16_t half, double sdrWhiteNits)
{
    const double linear = static_cast<double>(floatFromHalf(half)) * (ScrgbWhiteNits / sdrWhiteNits);
    if (std::isnan(linear) || linear <= 0.0) {
        return 0;
    }
    if (linear >= 1.0) {
        return 1023;
    }

    return static_cast<uint16_t>(std::lround(encodedFromLinear(linear) * 1023.0));
}

}  // namespace

TEST_CASE("Half-precision patterns decode to their float values")
{
    CHECK(floatFromHalf(0x0000) == 0.0f);
    CHECK(floatFromHalf(0x3C00) == 1.0f);
    CHECK(floatFromHalf(0x3800) == 0.5f);
    CHECK(floatFromHalf(0x4200) == 3.0f);
    CHECK(floatFromHalf(0xBC00) == -1.0f);
    CHECK(floatFromHalf(0x3555) == Catch::Approx(0.333251953125).epsilon(1e-9));
    CHECK(floatFromHalf(0x0001) == Catch::Approx(5.960464477539063e-08).epsilon(1e-9));  // smallest subnormal
    CHECK(floatFromHalf(0x03FF) == Catch::Approx(6.097555160522461e-05).epsilon(1e-9));  // largest subnormal
    CHECK(floatFromHalf(0x0400) == Catch::Approx(6.103515625e-05).epsilon(1e-9));        // smallest normal
    CHECK(floatFromHalf(0x7BFF) == 65504.0f);
    CHECK(std::isinf(floatFromHalf(0x7C00)));
    CHECK(std::isinf(floatFromHalf(0xFC00)));
    CHECK(std::isnan(floatFromHalf(0x7E00)));
}

TEST_CASE("Half decode round-trips an independent encode over every finite pattern")
{
    for (uint32_t pattern = 0; pattern < 0x10000u; ++pattern) {
        const auto half = static_cast<uint16_t>(pattern);
        if ((half & 0x7C00u) == 0x7C00u) {
            continue;  // infinities and NaNs
        }
        const float value = floatFromHalf(half);
        const uint16_t back = halfFromFloat(value);
        // -0 and +0 both decode to zero; only the sign differs.
        CHECK(((half & 0x7FFFu) == 0 ? (back & 0x7FFFu) == 0 : back == half));
    }
}

TEST_CASE("The sRGB encode inverts the decode the scopes use")
{
    CHECK(encodedFromLinear(0.0) == 0.0);
    CHECK(encodedFromLinear(1.0) == Catch::Approx(1.0).epsilon(1e-12));
    CHECK(encodedFromLinear(0.5) == Catch::Approx(0.7353569830524495).epsilon(1e-12));
    // Continuous at the linear toe.
    CHECK(encodedFromLinear(0.0031308) == Catch::Approx(0.040449936).epsilon(1e-6));
    for (int step = 0; step <= 1000; ++step) {
        const double encoded = step / 1000.0;
        CHECK(encodedFromLinear(linearFromEncoded(encoded)) == Catch::Approx(encoded).margin(1e-12));
    }
}

TEST_CASE("At the scRGB white, 1.0 is full scale and the mid-grey lands on its sRGB code")
{
    const ScrgbToDisplayCodes codes;
    CHECK(codes.sdrWhiteNits() == ScrgbWhiteNits);
    CHECK(codes.codeFor(0x3C00) == 1023);  // 1.0
    CHECK(codes.codeFor(0x3800) == 752);   // 0.5 linear encodes to 0.7354, times 1023
    CHECK(codes.codeFor(0x0000) == 0);
    CHECK(codes.codeFor(0x8000) == 0);     // -0
    CHECK(codes.codeFor(0xBC00) == 0);     // -1.0: out-of-gamut excursions read as black
    CHECK(codes.codeFor(0x7E00) == 0);     // NaN
    CHECK(codes.codeFor(0x7C00) == 1023);  // +inf
    CHECK(codes.codeFor(0x4200) == 1023);  // 3.0: brighter than SDR white saturates
    CHECK(codes.codeFor(0x0001) == 0);     // the smallest subnormal encodes below half a code
    // At or above SDR white is where the code saturates: exactly 1.0 counts,
    // the largest value below it does not, and nothing negative or undefined does.
    CHECK(codes.aboveWhite(0x3C00));
    CHECK(codes.aboveWhite(0x4200));
    CHECK(codes.aboveWhite(0x7C00));
    CHECK_FALSE(codes.aboveWhite(0x3BFF));
    CHECK_FALSE(codes.aboveWhite(0x3800));
    CHECK_FALSE(codes.aboveWhite(0xBC00));
    CHECK_FALSE(codes.aboveWhite(0x7E00));
}

TEST_CASE("The SDR white level rescales the whole table")
{
    ScrgbToDisplayCodes codes(240.0);
    CHECK(codes.sdrWhiteNits() == 240.0);
    CHECK(codes.codeFor(0x4200) == 1023);  // 3.0 is the SDR white at 240 nits
    CHECK(codes.aboveWhite(0x4200));
    CHECK_FALSE(codes.aboveWhite(0x3C00));  // 1.0 is a third of the way there
    // 1.0 at 240 nits is one third of SDR white: 1.055 * (1/3)^(1/2.4) - 0.055, times 1023.
    const auto oneThird = static_cast<uint16_t>(std::lround(encodedFromLinear(1.0 / 3.0) * 1023.0));
    CHECK(codes.codeFor(0x3C00) == oneThird);
    CHECK(oneThird > 600);
    CHECK(oneThird < 650);

    codes.setSdrWhiteNits(80.0);
    CHECK(codes.codeFor(0x3C00) == 1023);
    codes.setSdrWhiteNits(0.0);  // non-positive: back to the scRGB white
    CHECK(codes.sdrWhiteNits() == ScrgbWhiteNits);
    CHECK(codes.codeFor(0x3800) == 752);
}

TEST_CASE("Every half pattern matches the reference law and codes never decrease with brightness")
{
    for (const double nits : {80.0, 200.0, 240.0, 480.0}) {
        const ScrgbToDisplayCodes codes(nits);
        uint16_t previous = 0;
        for (uint32_t pattern = 0; pattern < 0x10000u; ++pattern) {
            const auto half = static_cast<uint16_t>(pattern);
            REQUIRE(codes.codeFor(half) == referenceCode(half, nits));
            // Positive finite patterns increase monotonically with their value.
            if (pattern < 0x7C00u) {
                REQUIRE(codes.codeFor(half) >= previous);
                previous = codes.codeFor(half);
            }
        }
    }
}

TEST_CASE("SDR content composed in scRGB comes back as its own codes")
{
    // The compositor hands out linear(code) * (sdrWhite / 80) for SDR content.
    // Dividing that out and encoding must return the code: exactly for a
    // 10-bit source with the white at 80 nits (Auto Color Management), and
    // within one code once half precision has to carry a value up to 3.0.
    for (const double nits : {80.0, 240.0}) {
        const ScrgbToDisplayCodes codes(nits);
        const double scale = nits / ScrgbWhiteNits;
        int offByOne = 0;
        for (int code = 0; code <= 1023; ++code) {
            const double linear = linearFromEncoded(code / 1023.0) * scale;
            const uint16_t half = halfFromFloat(static_cast<float>(linear));
            const int back = codes.codeFor(half);
            REQUIRE(std::abs(back - code) <= 1);
            offByOne += back != code ? 1 : 0;
        }
        if (nits == 80.0) {
            CHECK(offByOne == 0);
        }
        for (int code = 0; code <= 255; ++code) {
            const double linear = linearFromEncoded(code / 255.0) * scale;
            const uint16_t half = halfFromFloat(static_cast<float>(linear));
            const int expected = static_cast<int>(std::lround(code * 1023.0 / 255.0));
            REQUIRE(std::abs(static_cast<int>(codes.codeFor(half)) - expected) <= 1);
        }
    }
}

TEST_CASE("A row converts to packed ten-bit pixels the frame reader decodes")
{
    const ScrgbToDisplayCodes codes(80.0);
    // Three pixels: mid grey, pure red at full scale, and a value above white.
    const uint16_t halves[] = {0x3800, 0x3800, 0x3800, 0x3C00,   // 0.5, 0.5, 0.5, alpha 1.0
                               0x3C00, 0x0000, 0x0000, 0x3C00,   // 1.0, 0, 0
                               0x4200, 0x3555, 0x8000, 0x0000};  // 3.0, 0.333, -0, alpha 0
    std::vector<uint8_t> source(sizeof halves);
    std::memcpy(source.data(), halves, sizeof halves);
    std::vector<uint8_t> packed(12, 0xAA);
    const int above = codes.convertRow(source.data(), packed.data(), 3);
    CHECK(above == 2);  // the red pixel reaches white, the last one exceeds it

    const Sample grey = Argb2101010Pixels::read(packed.data());
    CHECK(grey.r == 752);
    CHECK(grey.g == 752);
    CHECK(grey.b == 752);
    const Sample red = Argb2101010Pixels::read(packed.data() + 4);
    CHECK(red.r == 1023);
    CHECK(red.g == 0);
    CHECK(red.b == 0);
    const Sample mixed = Argb2101010Pixels::read(packed.data() + 8);
    CHECK(mixed.r == 1023);
    CHECK(mixed.g == codes.codeFor(0x3555));
    CHECK(mixed.b == 0);
    // Alpha is written opaque regardless of the source alpha.
    for (int pixel = 0; pixel < 3; ++pixel) {
        CHECK((packed[static_cast<std::size_t>(pixel) * 4 + 3] & 0xC0u) == 0xC0u);
    }

    // A zero-width row touches nothing and counts nothing.
    std::vector<uint8_t> untouched(4, 0x55);
    CHECK(codes.convertRow(source.data(), untouched.data(), 0) == 0);
    CHECK(untouched == std::vector<uint8_t>(4, 0x55));
}

TEST_CASE("The converted row reads as the same colour on the 0..255 scale a frame reports")
{
    const ScrgbToDisplayCodes codes(80.0);
    const uint16_t halves[] = {0x3800, 0x3800, 0x3800, 0x3C00};
    std::vector<uint8_t> source(sizeof halves);
    std::memcpy(source.data(), halves, sizeof halves);
    std::vector<uint8_t> packed(4);
    codes.convertRow(source.data(), packed.data(), 1);
    FrameView frame{packed.data(), 4, 1, 1, ColorSpaceHint::Srgb, 1};
    frame.format = PixelFormat::Argb2101010;
    const FloatColor colour = frame.srgbAt(0, 0);
    CHECK(colour.r == Catch::Approx(752.0f * 255.0f / 1023.0f));
    CHECK(colour.r == Catch::Approx(187.5f).margin(0.1f));
}
