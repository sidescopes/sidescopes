#include "web/demo_layout.h"

#include <algorithm>

namespace sidescopes {

ShellLayout layoutFor(const LayoutPoint& origin, const LayoutPoint& size)
{
    if (size.x >= size.y * WideEnough) {
        const float appWidth = std::min(AppWidthPoints, size.x * 0.5f);

        return ShellLayout{origin, LayoutPoint{size.x - appWidth, size.y},
                           LayoutPoint{origin.x + size.x - appWidth + AppMargin, origin.y + AppMargin},
                           LayoutPoint{appWidth - AppMargin * 2.0f, size.y - AppMargin * 2.0f}};
    }
    const float appHeight = size.y * AppHeightShare;

    return ShellLayout{origin, LayoutPoint{size.x, size.y - appHeight},
                       LayoutPoint{origin.x + AppMargin, origin.y + size.y - appHeight + AppMargin},
                       LayoutPoint{size.x - AppMargin * 2.0f, appHeight - AppMargin * 2.0f}};
}

}  // namespace sidescopes
