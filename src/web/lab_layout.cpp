#include "web/lab_layout.h"

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

std::optional<LayoutRect> picturePixelsUnderRegion(const LayoutRect& desktopRegion, const LayoutRect& pictureOnDesktop,
                                                   const LayoutPoint& picturePixels)
{
    if (pictureOnDesktop.size.x <= 0.0f || pictureOnDesktop.size.y <= 0.0f || picturePixels.x <= 0.0f ||
        picturePixels.y <= 0.0f) {
        return std::nullopt;
    }
    const float left = std::max(desktopRegion.position.x, pictureOnDesktop.position.x);
    const float top = std::max(desktopRegion.position.y, pictureOnDesktop.position.y);
    const float right = std::min(desktopRegion.position.x + desktopRegion.size.x,
                                 pictureOnDesktop.position.x + pictureOnDesktop.size.x);
    const float bottom = std::min(desktopRegion.position.y + desktopRegion.size.y,
                                  pictureOnDesktop.position.y + pictureOnDesktop.size.y);
    if (right <= left || bottom <= top) {
        return std::nullopt;
    }
    const float scaleX = picturePixels.x / pictureOnDesktop.size.x;
    const float scaleY = picturePixels.y / pictureOnDesktop.size.y;

    return LayoutRect{
        LayoutPoint{(left - pictureOnDesktop.position.x) * scaleX, (top - pictureOnDesktop.position.y) * scaleY},
        LayoutPoint{(right - left) * scaleX, (bottom - top) * scaleY}};
}

}  // namespace sidescopes
