#include <X11/Xlib.h>

#include <catch2/catch_test_macros.hpp>

#include "platform/linux/x11_pixels.h"

using namespace sidescopes;

namespace {

/// A one-pixel XImage carrying just the fields the unpackers read: the layout
/// and the channel masks. No X server and no allocation - the struct is public.
XImage imageWith(int bitsPerPixel, int byteOrder, unsigned long red, unsigned long green, unsigned long blue)
{
    XImage image{};
    image.width = 1;
    image.height = 1;
    image.bits_per_pixel = bitsPerPixel;
    image.byte_order = byteOrder;
    image.red_mask = red;
    image.green_mask = green;
    image.blue_mask = blue;

    return image;
}

}  // namespace

TEST_CASE("the ordinary 32-bit little-endian BGRA visual is the direct-copy layout")
{
    const XImage image = imageWith(32, LSBFirst, 0x00FF0000, 0x0000FF00, 0x000000FF);
    CHECK(isDirectBgraLayout(&image));
}

TEST_CASE("a big-endian, non-32-bit, or RGB-ordered visual is not the direct layout")
{
    const XImage bigEndian = imageWith(32, MSBFirst, 0x00FF0000, 0x0000FF00, 0x000000FF);
    const XImage sixteenBit = imageWith(16, LSBFirst, 0xF800, 0x07E0, 0x001F);
    const XImage rgbOrder = imageWith(32, LSBFirst, 0x000000FF, 0x0000FF00, 0x00FF0000);
    CHECK_FALSE(isDirectBgraLayout(&bigEndian));
    CHECK_FALSE(isDirectBgraLayout(&sixteenBit));
    CHECK_FALSE(isDirectBgraLayout(&rgbOrder));
}

TEST_CASE("unpack recovers each channel by its mask")
{
    const XImage image = imageWith(32, LSBFirst, 0x00FF0000, 0x0000FF00, 0x000000FF);
    uint8_t blue = 0;
    uint8_t green = 0;
    uint8_t red = 0;
    unpackPixelBgra(&image, 0x00AABBCCUL, blue, green, red);
    CHECK(red == 0xAA);
    CHECK(green == 0xBB);
    CHECK(blue == 0xCC);
}

TEST_CASE("unpack of a channel wider than eight bits keeps the high bits")
{
    // A 10-bit channel (a 30-bpc visual). The significant bits are the high
    // ones, so 0b10'0000'0001 must read as 0x80, not the low byte 0x01 - a
    // near-max value reading as near-zero was the bug.
    const XImage image = imageWith(32, LSBFirst, 0x000003FF, 0, 0);
    uint8_t blue = 0;
    uint8_t green = 0;
    uint8_t red = 0;
    unpackPixelBgra(&image, 0x201UL, blue, green, red);
    CHECK(red == 0x80);
}

TEST_CASE("unpack scales a narrow channel up to eight bits")
{
    // 5-6-5 RGB: red and blue full, green zero.
    const XImage image = imageWith(16, LSBFirst, 0xF800, 0x07E0, 0x001F);
    uint8_t blue = 0;
    uint8_t green = 0;
    uint8_t red = 0;
    unpackPixelBgra(&image, 0xF81FUL, blue, green, red);
    CHECK(red == 0xFF);
    CHECK(blue == 0xFF);
    CHECK(green == 0);
}
