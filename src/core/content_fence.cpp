#include "core/content_fence.h"

#include <algorithm>

namespace sidescopes {
namespace {

IntRect Intersect(const IntRect& a, const IntRect& b) {
    const int x0 = std::max(a.x, b.x);
    const int y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.x + a.width, b.x + b.width);
    const int y1 = std::min(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0) return IntRect{};
    return IntRect{x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

ContentFence FenceContent(const IntRect& frame_bounds, const IntRect& main_window,
                          const std::vector<IntRect>& chrome, int slack) {
    ContentFence fence;
    fence.content = Intersect(frame_bounds, main_window);
    if (fence.content.width <= 0 || fence.content.height <= 0) fence.content = frame_bounds;

    for (bool trimmed = true; trimmed;) {
        trimmed = false;
        for (const IntRect& piece : chrome) {
            IntRect& content = fence.content;
            const IntRect overlap = Intersect(content, piece);
            if (overlap.width <= 0 || overlap.height <= 0) continue;
            const bool full_width = overlap.width * 20 >= content.width * 17;
            const bool full_height = overlap.height * 20 >= content.height * 17;
            IntRect next = content;
            if (full_width && piece.y <= content.y + slack) {
                next.height = content.y + content.height - (overlap.y + overlap.height);
                next.y = overlap.y + overlap.height;
            } else if (full_width && piece.y + piece.height >= content.y + content.height - slack) {
                next.height = overlap.y - content.y;
            } else if (full_height && piece.x <= content.x + slack) {
                next.width = content.x + content.width - (overlap.x + overlap.width);
                next.x = overlap.x + overlap.width;
            } else if (full_height && piece.x + piece.width >= content.x + content.width - slack) {
                next.width = overlap.x - content.x;
            } else {
                continue;
            }
            if (next.width > 0 && next.height > 0 &&
                (next.x != content.x || next.y != content.y || next.width != content.width ||
                 next.height != content.height)) {
                content = next;
                trimmed = true;
            }
        }
    }

    for (const IntRect& piece : chrome) {
        const IntRect overlap = Intersect(fence.content, piece);
        if (overlap.width <= 0 || overlap.height <= 0) continue;
        fence.occluders.push_back(IntRect{overlap.x - fence.content.x, overlap.y - fence.content.y,
                                          overlap.width, overlap.height});
    }
    return fence;
}

}  // namespace sidescopes
