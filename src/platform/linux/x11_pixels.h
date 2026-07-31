#pragma once

#include <X11/Xlib.h>

#include <cstdint>

namespace sidescopes {

/// Whether an XImage's pixels are already the BGRA byte order the scopes read,
/// so a row can be copied straight out with no per-pixel work. True for the
/// ordinary truecolor visual every desktop uses: 32 bits a pixel, little-
/// endian, with blue in the low byte. A frame in this layout stores each pixel
/// as the bytes B, G, R, X - exactly PixelFormat::Bgra8 with an undefined
/// fourth byte the scopes ignore.
[[nodiscard]] inline bool isDirectBgraLayout(const XImage* image)
{
    return image != nullptr && image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
           image->blue_mask == 0x000000FFUL && image->green_mask == 0x0000FF00UL && image->red_mask == 0x00FF0000UL;
}

/// One pixel out of an XImage as BGRA channels. XGetImage on a truecolor
/// visual packs the channels by the visual's masks; shifting by each mask's low
/// bit recovers them without assuming a byte order, and scaling by the mask's
/// width brings any channel depth up to eight. The slow path behind
/// isDirectBgraLayout - correct for every visual, so an exotic one still reads.
inline void unpackPixelBgra(const XImage* image, unsigned long pixel, uint8_t& blue, uint8_t& green, uint8_t& red)
{
    const auto channel = [pixel](unsigned long mask) -> uint8_t {
        if (mask == 0) {
            return 0;
        }
        unsigned long value = pixel & mask;
        while ((mask & 1) == 0) {
            mask >>= 1;
            value >>= 1;
        }
        // A channel wider than eight bits (a 10-bit visual) keeps its HIGH eight
        // - the significant bits - not its low ones, so a bright value does not
        // read as dark. A narrower channel is scaled up to eight.
        while (mask > 0xFF) {
            mask >>= 1;
            value >>= 1;
        }
        while (mask < 0xFF) {
            mask = (mask << 1) | 1;
            value = (value << 1) | (value & 1);
        }
        return static_cast<uint8_t>(value & 0xFF);
    };
    red = channel(image->red_mask);
    green = channel(image->green_mask);
    blue = channel(image->blue_mask);
}

}  // namespace sidescopes
