#include "app/window_place.h"

#include <algorithm>
#include <array>
#include <limits>

namespace sidescopes {
namespace {

// The chrome the platform draws around the window, in 100%-scale units: the
// margins are deliberately generous, since a mask a little too large costs a
// band of screen nobody is scoping and a mask too small costs a redraw loop.
constexpr float MarginLeft = 8.0f;
constexpr float MarginTop = 42.0f;
constexpr float MarginWidth = 16.0f;
constexpr float MarginHeight = 58.0f;

constexpr double StarterMargin = 3.0;
constexpr double StarterGap = 3.0;
constexpr double StarterWidth = 36.0;
constexpr double StarterHeight = 34.0;

enum class StarterSide
{
    Left,
    Right,
    Above,
    Below,
};

struct StarterArea
{
    StarterSide side;
    double left;
    double top;
    double right;
    double bottom;
};

double percent(double value, double origin, double length)
{
    return length > 0.0 ? (value - origin) / length * 100.0 : 0.0;
}

}  // namespace

std::optional<uint32_t> displayUnderWindow(const WindowPlacement& window)
{
    return displayAtPoint(DesktopPoint{window.x + window.width / 2.0, window.y + window.height / 2.0});
}

RegionOfInterest starterGlobalRegion(const WindowPlacement& window, const DisplayGeometry& display)
{
    if (display.widthPoints <= 0.0 || display.heightPoints <= 0.0) {
        return RegionOfInterest{12.0, 12.0, 48.0, 46.0};
    }

    const double windowLeft = std::clamp(percent(window.x, display.originX, display.widthPoints), 0.0, 100.0);
    const double windowTop = std::clamp(percent(window.y, display.originY, display.heightPoints), 0.0, 100.0);
    const double windowRight =
        std::clamp(percent(window.x + window.width, display.originX, display.widthPoints), 0.0, 100.0);
    const double windowBottom =
        std::clamp(percent(window.y + window.height, display.originY, display.heightPoints), 0.0, 100.0);
    const double limit = 100.0 - StarterMargin;
    const std::array<StarterArea, 4> areas{
        StarterArea{StarterSide::Left, StarterMargin, StarterMargin, std::max(StarterMargin, windowLeft - StarterGap),
                    limit},
        StarterArea{StarterSide::Right, std::min(limit, windowRight + StarterGap), StarterMargin, limit, limit},
        StarterArea{StarterSide::Above, StarterMargin, StarterMargin, limit,
                    std::max(StarterMargin, windowTop - StarterGap)},
        StarterArea{StarterSide::Below, StarterMargin, std::min(limit, windowBottom + StarterGap), limit, limit},
    };

    const double windowCentreX = (windowLeft + windowRight) * 0.5;
    const double windowCentreY = (windowTop + windowBottom) * 0.5;
    RegionOfInterest best{12.0, 12.0, 48.0, 46.0};
    double bestScore = -std::numeric_limits<double>::infinity();
    for (const StarterArea& area : areas) {
        const double availableWidth = std::max(0.0, area.right - area.left);
        const double availableHeight = std::max(0.0, area.bottom - area.top);
        if (availableWidth <= 0.0 || availableHeight <= 0.0) {
            continue;
        }
        const double width = std::min(StarterWidth, availableWidth);
        const double height = std::min(StarterHeight, availableHeight);
        const bool horizontal = area.side == StarterSide::Left || area.side == StarterSide::Right;
        const double fit = std::min(width / StarterWidth, height / StarterHeight);
        // A full-size region wins first; where several fit, prefer a side of
        // the tall SideScopes window, then the roomier rectangle.
        const double score = fit * 1000.0 + (horizontal ? 10.0 : 0.0) + availableWidth * availableHeight / 10000.0;
        if (score <= bestScore) {
            continue;
        }

        double left = std::clamp(windowCentreX - width * 0.5, area.left, area.right - width);
        double top = std::clamp(windowCentreY - height * 0.5, area.top, area.bottom - height);
        if (area.side == StarterSide::Left) {
            left = area.right - width;
        } else if (area.side == StarterSide::Right) {
            left = area.left;
        } else if (area.side == StarterSide::Above) {
            top = area.bottom - height;
        } else {
            top = area.top;
        }
        best = RegionOfInterest{left, top, left + width, top + height};
        bestScore = score;
    }

    return best;
}

IntRect selfWindowMask(const WindowPlacement& window, const DisplayGeometry& display, int displayWidth,
                       int displayHeight, float uiScale)
{
    const double scaleX = displayWidth / display.widthPoints;
    const double scaleY = displayHeight / display.heightPoints;

    return IntRect{static_cast<int>((window.x - display.originX - MarginLeft * uiScale) * scaleX),
                   static_cast<int>((window.y - display.originY - MarginTop * uiScale) * scaleY),
                   static_cast<int>((static_cast<float>(window.width) + MarginWidth * uiScale) * scaleX),
                   static_cast<int>((static_cast<float>(window.height) + MarginHeight * uiScale) * scaleY)};
}

}  // namespace sidescopes
