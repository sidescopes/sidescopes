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

    /// Whether the point (@p pointX, @p pointY) lies inside, taking the
    /// rectangle as half-open so adjacent rectangles never both claim a point.
    [[nodiscard]] bool contains(int pointX, int pointY) const
    {
        return pointX >= x && pointY >= y && pointX < x + width && pointY < y + height;
    }
};

/// 8-bit display-encoded color, as captured.
struct Color
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

/// How a captured frame packs its pixels. Both layouts are four bytes per
/// pixel, so a frame buffer costs the same either way and the capture backends
/// copy them identically.
enum class PixelFormat
{
    /// Blue, green, red, alpha, one byte each. What every backend delivered
    /// before 10-bit capture, and still the fallback everywhere.
    Bgra8,
    /// Ten bits per channel packed into one little-endian 32-bit word: alpha
    /// in bits 30-31, red 20-29, green 10-19, blue 0-9.
    Argb2101010
};

/// One pixel's channels on the scale its frame's format resolves - 0..255 for
/// Bgra8, 0..1023 for Argb2101010. Carrying the native codes rather than
/// promoting them keeps an 8-bit frame's arithmetic exactly what it always
/// was; see levelIn below for the 0..255 scale the scopes bin on.
struct Sample
{
    uint16_t r = 0;
    uint16_t g = 0;
    uint16_t b = 0;
};

/// Fractional bits a scope resolves below one 0..255 level.
///
/// Only the scopes that project a code into a CONTINUOUS position ask for any.
/// A scope whose axis is the code itself - the waveform's level, the histogram's
/// bin - is bounded by how many bins it holds, not by how many bits arrived, so
/// it takes the whole level and a finer input changes nothing it can draw.
/// Measured; see the level table in the notes.
inline constexpr int WholeLevelBits = 0;
inline constexpr int VectorscopeLevelBits = 4;

/// Reads Bgra8 pixels. A compile-time policy rather than a runtime branch: the
/// accumulate loops instantiate one of these per pass, so the hot path holds no
/// per-pixel test of the format.
struct Bgra8Pixels
{
    static constexpr int MaxCode = 255;
    static constexpr PixelFormat Format = PixelFormat::Bgra8;

    [[nodiscard]] static Sample read(const uint8_t* pixel)
    {
        return Sample{pixel[2], pixel[1], pixel[0]};
    }
};

/// Reads Argb2101010 pixels. The word is assembled from bytes rather than
/// read through a uint32_t so the unpack does not depend on the host's
/// alignment rules; every compiler folds it back into a single load.
struct Argb2101010Pixels
{
    static constexpr int MaxCode = 1023;
    static constexpr PixelFormat Format = PixelFormat::Argb2101010;

    [[nodiscard]] static Sample read(const uint8_t* pixel)
    {
        const uint32_t word = static_cast<uint32_t>(pixel[0]) | static_cast<uint32_t>(pixel[1]) << 8 |
                              static_cast<uint32_t>(pixel[2]) << 16 | static_cast<uint32_t>(pixel[3]) << 24;

        return Sample{static_cast<uint16_t>(word >> 20 & 0x3FFu), static_cast<uint16_t>(word >> 10 & 0x3FFu),
                      static_cast<uint16_t>(word & 0x3FFu)};
    }
};

/// One channel as the 0..255 level a scope bins on, carrying @p FractionBits
/// bits below it. An 8-bit code maps to exactly @c code << FractionBits, so its
/// fraction is always zero and an 8-bit frame bins precisely where it always
/// did; a 10-bit code lands between two levels and the scopes splat it across
/// both. Full scale maps to 255 at either depth, so white stays white.
/// Rounds rather than truncates, which only a deeper frame can notice: an
/// 8-bit code converts exactly, so its result is unchanged either way, while
/// truncation would bias every 10-bit sample half a step toward black.
template <typename Pixels, int FractionBits>
[[nodiscard]] constexpr int levelIn(int code)
{
    constexpr int One = 1 << FractionBits;

    if constexpr (Pixels::MaxCode == 255) {
        return code * One;
    } else {
        return (code * (255 * One) + Pixels::MaxCode / 2) / Pixels::MaxCode;
    }
}

/// Floating-point RGB on the 0..255 scale. Marker and indicator paths stay in
/// floating point end to end: quantizing intermediate values makes a smoothed
/// marker dither between adjacent scope bins while it settles.
struct FloatColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

/// Non-owning view of one captured frame, four bytes per pixel in whichever
/// layout @c format names, rows top-down. The producer guarantees the pixels
/// stay valid for the duration of the call that received the view.
struct FrameView
{
    const uint8_t* pixels = nullptr;
    int strideBytes = 0;
    int width = 0;
    int height = 0;
    ColorSpaceHint colorSpace = ColorSpaceHint::Unknown;
    /// Which frame this is, counted up by the producer. Both capture backends
    /// hand out the same buffer over and over, so this and the pointer TOGETHER
    /// are what identify the content: a scope that has already binned this
    /// pointer at this sequence takes the frame as unchanged and reuses what it
    /// computed. A producer that reused a number for new pixels would hand it a
    /// stale reading.
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
    /// Last so that every existing construction keeps naming the same fields.
    /// Defaulting to Bgra8 is what makes that safe: a producer that says
    /// nothing produces exactly the frame it always did.
    PixelFormat format = PixelFormat::Bgra8;

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

    /// Whether every pixel of @p displayRect, stated in display pixels, is in
    /// this frame. Always true of an uncropped frame for anything inside its
    /// display; false where a narrowing has left part of the rectangle out.
    ///
    /// A crop trails the region it was made for by the reconfiguration's own
    /// latency, so a frame in hand can be the crop for somewhere the region has
    /// already left. Measuring the region against it anyway would silently clip
    /// it to the overlap - a reading of part of the region, indistinguishable
    /// downstream from a reading of all of it.
    [[nodiscard]] bool carries(IntRect displayRect) const
    {
        const IntRect local = fromDisplay(displayRect);

        return local.empty() || local == local.clampedTo(width, height);
    }

    /// The pixel's bytes, in whatever layout @c format names. Callers that go
    /// through this must decode it themselves; anything wanting colour should
    /// use sampleAt or srgbAt, which are format-independent.
    [[nodiscard]] const uint8_t* rawPixelAt(int px, int py) const
    {
        return pixels + static_cast<std::size_t>(py) * strideBytes + static_cast<std::size_t>(px) * 4;
    }

    /// The highest code a channel of this frame can hold.
    [[nodiscard]] int maxCode() const
    {
        return format == PixelFormat::Argb2101010 ? Argb2101010Pixels::MaxCode : Bgra8Pixels::MaxCode;
    }

    /// The pixel's channels on this frame's own scale, 0..maxCode().
    [[nodiscard]] Sample sampleAt(int px, int py) const
    {
        const uint8_t* pixel = rawPixelAt(px, py);

        return format == PixelFormat::Argb2101010 ? Argb2101010Pixels::read(pixel) : Bgra8Pixels::read(pixel);
    }

    /// The pixel on the 0..255 display scale every readout, marker and pin
    /// speaks, keeping a 10-bit frame's sub-code precision in the fraction.
    /// Exact for an 8-bit frame.
    [[nodiscard]] FloatColor srgbAt(int px, int py) const
    {
        const Sample sample = sampleAt(px, py);
        const float scale = 255.0f / static_cast<float>(maxCode());

        return FloatColor{static_cast<float>(sample.r) * scale, static_cast<float>(sample.g) * scale,
                          static_cast<float>(sample.b) * scale};
    }
};

/// Whether a capture plane holding @p availableBytes can supply @p rows rows
/// of @p rowBytes, laid @p stride bytes apart.
///
/// A producer may hand over a chunk shorter than the negotiated frame - a
/// partial frame, or a buffer it has not finished filling - and copying the
/// whole frame out of one reads past the mapping. The LAST row decides it:
/// every row before it is covered by the strides in front of it, so the need
/// is (rows - 1) strides plus one row, not rows whole strides. Using the
/// latter refuses honest buffers whose final row is not padded.
[[nodiscard]] constexpr bool planeCoversRows(std::size_t availableBytes, int stride, int rowBytes, int rows)
{
    if (stride <= 0 || rowBytes <= 0 || rows <= 0 || rowBytes > stride) {
        return false;
    }
    const std::size_t needed =
        static_cast<std::size_t>(rows - 1) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(rowBytes);

    return availableBytes >= needed;
}

}  // namespace sidescopes
