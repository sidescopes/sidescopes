#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "web/image_adjust.h"

namespace sidescopes {
namespace {

/// One BGRA pixel in, one out. The lab's picture is a million of these and
/// nothing about the maths is neighbour-dependent, so a single pixel is the
/// whole unit under test.
/// Channels held as int, deliberately: Catch2 prints a uint8_t as a
/// character, so a failed comparison reads `'\xB0' > 180` and tells the
/// reader nothing about what the arithmetic did.
struct Pixel
{
    int blue = 0;
    int green = 0;
    int red = 0;
    int alpha = 255;
};

[[nodiscard]] Pixel adjusted(const Pixel& source, const ImageAdjustments& adjustments)
{
    const std::array<uint8_t, 4> in{static_cast<uint8_t>(source.blue), static_cast<uint8_t>(source.green),
                                    static_cast<uint8_t>(source.red), static_cast<uint8_t>(source.alpha)};
    std::array<uint8_t, 4> out{};
    applyAdjustments(in.data(), out.data(), 1, adjustments);

    return Pixel{out[0], out[1], out[2], out[3]};
}

constexpr Pixel MidGrey{128, 128, 128, 255};

}  // namespace

TEST_CASE("A neutral set returns the picture byte for byte")
{
    // Not "very close". A visitor who puts every control back to zero is owed
    // the photograph they started with, and a scope that settled somewhere
    // near it would be reporting on this arithmetic rather than on the
    // picture.
    const std::vector<uint8_t> source{12, 200, 47, 255, 0, 0, 0, 255, 255, 255, 255, 128};
    std::vector<uint8_t> out(source.size(), 0);
    applyAdjustments(source.data(), out.data(), 3, ImageAdjustments{});

    CHECK(out == source);
    CHECK(ImageAdjustments{}.neutral());
}

TEST_CASE("Adjustments read the source, so dragging a control does not accumulate")
{
    // THE failure this design exists to prevent: applying over the previous
    // result would degrade the picture as a slider is dragged back and forth,
    // and the scopes would show that degradation as though it belonged to the
    // photograph.
    ImageAdjustments warm;
    warm.exposure = 0.5f;
    warm.saturation = 0.4f;

    const Pixel once = adjusted(MidGrey, warm);
    const Pixel twice = adjusted(MidGrey, warm);
    CHECK(once.red == twice.red);
    CHECK(once.green == twice.green);
    CHECK(once.blue == twice.blue);

    // And going back to neutral restores the original exactly, rather than
    // landing wherever the round trip left it.
    const Pixel restored = adjusted(MidGrey, ImageAdjustments{});
    CHECK(restored.red == MidGrey.red);
    CHECK(restored.green == MidGrey.green);
    CHECK(restored.blue == MidGrey.blue);
}

TEST_CASE("A stop of exposure is a doubling of light, not of the encoded value")
{
    // Done in gamma space, +1 stop would take 128 to 255 and clip a mid grey.
    // Linear-light exposure preserves the midtone instead.
    ImageAdjustments up;
    up.exposure = 1.0f;
    const Pixel brighter = adjusted(MidGrey, up);

    // 128 is about 21.6% of full light; twice that is 43%, which encodes back
    // to 176. Done in gamma space instead it would land at 255 and clip a mid
    // grey, which is the mistake this pins.
    CHECK(brighter.green > 172);
    CHECK(brighter.green < 180);
}

TEST_CASE("Exposure clips at white rather than wrapping")
{
    ImageAdjustments blown;
    blown.exposure = 3.0f;
    const Pixel white = adjusted(Pixel{200, 200, 200, 255}, blown);

    CHECK(white.red == 255);
    CHECK(white.green == 255);
    CHECK(white.blue == 255);
}

TEST_CASE("Warming raises red and lowers blue")
{
    // The vectorscope lesson: a cast moves the whole cloud in one direction.
    ImageAdjustments warm;
    warm.temperature = 1.0f;
    const Pixel result = adjusted(MidGrey, warm);

    CHECK(result.red > MidGrey.red);
    CHECK(result.blue < MidGrey.blue);

    ImageAdjustments cool;
    cool.temperature = -1.0f;
    const Pixel cooled = adjusted(MidGrey, cool);
    CHECK(cooled.red < MidGrey.red);
    CHECK(cooled.blue > MidGrey.blue);
}

TEST_CASE("Tint moves green against magenta")
{
    ImageAdjustments magenta;
    magenta.tint = 1.0f;
    const Pixel result = adjusted(MidGrey, magenta);

    CHECK(result.green < MidGrey.green);
    CHECK(result.red > MidGrey.red);
    CHECK(result.blue > MidGrey.blue);
}

TEST_CASE("Saturation at its floor leaves every channel equal")
{
    // What the vectorscope shows as a collapse onto the centre point.
    ImageAdjustments grey;
    grey.saturation = -1.0f;
    const Pixel result = adjusted(Pixel{40, 160, 220, 255}, grey);

    CHECK(result.red == result.green);
    CHECK(result.green == result.blue);
}

TEST_CASE("Saturation pushes colour away from grey without moving its luma far")
{
    const Pixel source{40, 160, 220, 255};
    ImageAdjustments richer;
    richer.saturation = 0.6f;
    const Pixel result = adjusted(source, richer);

    // The channel furthest from grey goes further; the one nearest comes in.
    CHECK(result.red > source.red);
    CHECK(result.blue < source.blue);
}

TEST_CASE("The two ends of the range move independently")
{
    // Which is the whole reason for having highlights and shadows as well as
    // exposure: lifting the shadows must leave a highlight where it was.
    ImageAdjustments lift;
    lift.shadows = 1.0f;

    const Pixel dark = adjusted(Pixel{40, 40, 40, 255}, lift);
    const Pixel bright = adjusted(Pixel{230, 230, 230, 255}, lift);

    CHECK(dark.green > 40);
    CHECK(bright.green >= 228);
    CHECK(bright.green <= 232);
}

TEST_CASE("Contrast pivots on mid grey")
{
    ImageAdjustments harder;
    harder.contrast = 0.5f;

    // The pivot itself barely moves; the ends move apart around it.
    const Pixel middle = adjusted(MidGrey, harder);
    CHECK(middle.green >= 126);
    CHECK(middle.green <= 130);

    CHECK(adjusted(Pixel{80, 80, 80, 255}, harder).green < 80);
    CHECK(adjusted(Pixel{180, 180, 180, 255}, harder).green > 180);
}

TEST_CASE("Alpha is carried through untouched")
{
    ImageAdjustments busy;
    busy.exposure = 1.0f;
    busy.saturation = -0.5f;

    CHECK(adjusted(Pixel{10, 20, 30, 77}, busy).alpha == 77);
}

}  // namespace sidescopes
