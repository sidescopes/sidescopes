#include "core/frame.h"

#include <algorithm>

namespace sidescopes {

IntRect IntRect::ClampedTo(int frame_width, int frame_height) const {
    const int left = std::max(x, 0);
    const int top = std::max(y, 0);
    const int right = std::min(x + width, frame_width);
    const int bottom = std::min(y + height, frame_height);
    return IntRect{left, top, right - left, bottom - top};
}

}  // namespace sidescopes
