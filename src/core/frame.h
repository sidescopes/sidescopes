#pragma once

#include <cstddef>
#include <cstdint>

namespace sidescopes {

/// What the capture backend believes about the pixel encoding it delivers.
/// The scope math currently treats everything as sRGB; the hint records what
/// actually arrived so a color-managed pipeline can build on it later.
enum class ColorSpaceHint
{
    Unknown,
    Srgb,
    DisplayP3,
    DisplayProfile
};

struct IntRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool empty() const
    {
        return width <= 0 || height <= 0;
    }

    [[nodiscard]] bool operator==(const IntRect& other) const
    {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }

    /// Intersection with the rectangle [0, 0, frameWidth, frameHeight). The
    /// result may be empty; callers must handle that.
    [[nodiscard]] IntRect clampedTo(int frameWidth, int frameHeight) const;
};

/// 8-bit display-encoded color, as captured.
struct Color
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

/// Floating-point RGB on the 0..255 scale. Marker and indicator paths stay in
/// floating point end to end: quantizing intermediate values makes a smoothed
/// marker dither between adjacent scope bins while it settles.
struct FloatColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

/// Non-owning view of one captured frame: BGRA, 8 bits per channel, rows
/// top-down. The producer guarantees the pixels stay valid for the duration
/// of the call that received the view.
struct FrameView
{
    const uint8_t* bgra = nullptr;
    int strideBytes = 0;
    int width = 0;
    int height = 0;
    ColorSpaceHint colorSpace = ColorSpaceHint::Unknown;
    uint64_t sequence = 0;
    /// Where this frame's top-left pixel sits on the display it came from, and
    /// how large that display is in pixels. A frame covering its whole display
    /// leaves all four at zero, which reads as "the frame IS the display".
    ///
    /// A capture narrowed to a sub-rectangle sets them, so a region expressed
    /// against the display still resolves to the same content: the region is
    /// measured in display pixels and then mapped through fromDisplay. Without
    /// this a narrowed frame would silently re-measure a percentage region
    /// against the crop, and every scope would read the wrong pixels for as
    /// long as it took the two to agree again.
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;

    /// The display's pixel extents, which for an uncropped frame are its own.
    [[nodiscard]] int displayWidth() const
    {
        return sourceWidth > 0 ? sourceWidth : width;
    }

    [[nodiscard]] int displayHeight() const
    {
        return sourceHeight > 0 ? sourceHeight : height;
    }

    /// Whether this frame covers less than the display it came from.
    [[nodiscard]] bool cropped() const
    {
        return sourceX != 0 || sourceY != 0 || displayWidth() != width || displayHeight() != height;
    }

    /// Moves @p rect from display pixels into this frame's own pixels. The
    /// identity for an uncropped frame.
    [[nodiscard]] IntRect fromDisplay(IntRect rect) const
    {
        return IntRect{rect.x - sourceX, rect.y - sourceY, rect.width, rect.height};
    }

    [[nodiscard]] const uint8_t* pixelAt(int px, int py) const
    {
        return bgra + static_cast<std::size_t>(py) * strideBytes + static_cast<std::size_t>(px) * 4;
    }

    [[nodiscard]] Color colorAt(int px, int py) const
    {
        const uint8_t* pixel = pixelAt(px, py);
        return Color{pixel[2], pixel[1], pixel[0]};
    }
};

}  // namespace sidescopes
