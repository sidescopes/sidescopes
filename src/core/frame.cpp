#include "core/frame.h"

#include <algorithm>
#include <cstring>

namespace sidescopes {

IntRect IntRect::clampedTo(int frameWidth, int frameHeight) const
{
    const int left = std::max(x, 0);
    const int top = std::max(y, 0);
    const int right = std::min(x + width, frameWidth);
    const int bottom = std::min(y + height, frameHeight);
    return IntRect{left, top, right - left, bottom - top};
}

std::vector<uint8_t> copyAsBgra8(const FrameView& frame)
{
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0 || frame.strideBytes < frame.width * 4) {
        return {};
    }

    const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * 4;
    std::vector<uint8_t> copy(rowBytes * static_cast<std::size_t>(frame.height));
    if (frame.format == PixelFormat::Bgra8) {
        for (int row = 0; row < frame.height; ++row) {
            std::memcpy(copy.data() + static_cast<std::size_t>(row) * rowBytes,
                        frame.pixels + static_cast<std::size_t>(row) * frame.strideBytes, rowBytes);
        }
        return copy;
    }

    for (int row = 0; row < frame.height; ++row) {
        for (int column = 0; column < frame.width; ++column) {
            const Sample sample = frame.sampleAt(column, row);
            uint8_t* pixel = copy.data() + (static_cast<std::size_t>(row) * frame.width + column) * 4;
            pixel[0] = static_cast<uint8_t>(levelIn<Argb2101010Pixels, WholeLevelBits>(sample.b));
            pixel[1] = static_cast<uint8_t>(levelIn<Argb2101010Pixels, WholeLevelBits>(sample.g));
            pixel[2] = static_cast<uint8_t>(levelIn<Argb2101010Pixels, WholeLevelBits>(sample.r));
            pixel[3] = 0xFF;
        }
    }
    return copy;
}

}  // namespace sidescopes
