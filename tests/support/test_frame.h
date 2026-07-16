#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/frame.h"
#include "core/frame_mailbox.h"

namespace sidescopes::test {

// One BGRA frame the scope, hashing, and marker tests build on, replacing the
// per-suite copies. The initial fill is load-bearing - some suites want a
// black canvas, others a white one - so it is a constructor argument. The
// mutators are the union of what those suites hand-rolled: a single-byte poke,
// a per-pixel color, and whole-frame or banded fills by row and by column.
struct TestFrame
{
    TestFrame(int width, int height, uint8_t fill = 0)
        : width(width),
          height(height)
    {
        pixels.assign(static_cast<std::size_t>(width) * height * 4, fill);
    }

    // Writes one byte at a pixel's base offset - all the region-hash tests
    // need to perturb content where only the fact of a change matters.
    void setPixel(int px, int py, uint8_t value)
    {
        pixels[(static_cast<std::size_t>(py) * width + px) * 4] = value;
    }

    // Writes a BGRA pixel from an RGB color; the alpha byte is left as filled.
    void setColor(int px, int py, Color color)
    {
        uint8_t* pixel = pixels.data() + (static_cast<std::size_t>(py) * width + px) * 4;
        pixel[0] = color.b;
        pixel[1] = color.g;
        pixel[2] = color.r;
    }

    // Fills the whole frame with one color.
    void fill(Color color)
    {
        fillRows(0, height, color);
    }

    // Fills the columns [x0, x1) across every row.
    void fill(int x0, int x1, Color color)
    {
        fillColumns(x0, x1, color);
    }

    // Fills the rows [y0, y1) across every column.
    void fillRows(int y0, int y1, Color color)
    {
        for (int py = y0; py < y1; ++py) {
            for (int px = 0; px < width; ++px) {
                setColor(px, py, color);
            }
        }
    }

    // Fills the columns [x0, x1) across every row.
    void fillColumns(int x0, int x1, Color color)
    {
        for (int py = 0; py < height; ++py) {
            for (int px = x0; px < x1; ++px) {
                setColor(px, py, color);
            }
        }
    }

    [[nodiscard]] FrameView view() const
    {
        return FrameView{pixels.data(), width * 4, width, height, ColorSpaceHint::Srgb, 1};
    }

    std::vector<uint8_t> pixels;
    int width;
    int height;
};

// An owned solid-color BGRA frame buffer for the mailbox and worker tests:
// stride packed to the width and fully opaque.
inline FrameBuffer makeSolidFrameBuffer(int width, int height, Color color, uint64_t sequence)
{
    FrameBuffer frame;
    frame.width = width;
    frame.height = height;
    frame.strideBytes = width * 4;
    frame.colorSpace = ColorSpaceHint::Srgb;
    frame.sequence = sequence;
    frame.data.resize(static_cast<std::size_t>(frame.strideBytes) * height);
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            uint8_t* pixel =
                frame.data.data() + static_cast<std::size_t>(py) * frame.strideBytes + static_cast<std::size_t>(px) * 4;
            pixel[0] = color.b;
            pixel[1] = color.g;
            pixel[2] = color.r;
            pixel[3] = 255;
        }
    }
    return frame;
}

}  // namespace sidescopes::test
