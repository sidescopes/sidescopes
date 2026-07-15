#include "core/frame.h"

#include <algorithm>

namespace sidescopes {

IntRect IntRect::clampedTo(int frameWidth, int frameHeight) const
{
    const int left = std::max(x, 0);
    const int top = std::max(y, 0);
    const int right = std::min(x + width, frameWidth);
    const int bottom = std::min(y + height, frameHeight);
    return IntRect{left, top, right - left, bottom - top};
}

}  // namespace sidescopes
