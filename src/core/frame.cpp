#include "core/frame.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace sidescopes {

IntRect IntRect::clampedTo(int frameWidth, int frameHeight) const
{
    if (empty() || frameWidth <= 0 || frameHeight <= 0) {
        return {};
    }
    const int left = std::clamp(x, 0, frameWidth);
    const int top = std::clamp(y, 0, frameHeight);
    const int right = static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(x) + width, left, frameWidth));
    const int bottom = static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(y) + height, top, frameHeight));
    return IntRect{left, top, right - left, bottom - top};
}

IntRect FrameView::fromDisplay(IntRect rect) const
{
    const auto local = [](int coordinate, int origin) {
        return static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(coordinate) - origin,
                                                    std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    };
    return IntRect{local(rect.x, sourceX), local(rect.y, sourceY), rect.width, rect.height};
}

std::vector<uint8_t> copyAsBgra8(const FrameView& frame)
{
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0 ||
        frame.strideBytes < static_cast<int64_t>(frame.width) * 4) {
        return {};
    }

    const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * 4;
    if (rowBytes > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(frame.height)) {
        return {};
    }
    std::vector<uint8_t> copy(rowBytes * static_cast<std::size_t>(frame.height));
    if (frame.format == PixelFormat::Bgra8) {
        for (int row = 0; row < frame.height; ++row) {
            std::memcpy(copy.data() + static_cast<std::size_t>(row) * rowBytes,
                        frame.pixels + static_cast<std::size_t>(row) * frame.strideBytes, rowBytes);
            for (std::size_t pixel = static_cast<std::size_t>(row) * rowBytes + 3;
                 pixel < static_cast<std::size_t>(row + 1) * rowBytes; pixel += 4) {
                copy[pixel] = 255;
            }
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
