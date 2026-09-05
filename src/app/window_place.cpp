#include "app/window_place.h"

#include <algorithm>
#include <array>
#include <cmath>
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
constexpr double StarterWindowInset = 0.02;

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

struct StarterSize
{
    double width;
    double height;
};

double percent(double value, double origin, double length)
{
    return length > 0.0 ? (value - origin) / length * 100.0 : 0.0;
}

StarterSize squareStarterSize(const DisplayGeometry& display)
{
    if (display.widthPoints <= 0.0 || display.heightPoints <= 0.0) {
        return StarterSize{StarterHeight, StarterHeight};
    }

    const double side =
        std::min(display.widthPoints * StarterWidth / 100.0, display.heightPoints * StarterHeight / 100.0);
    return StarterSize{side / display.widthPoints * 100.0, side / display.heightPoints * 100.0};
}

RegionOfInterest centeredStarterRegion(const StarterSize& size)
{
    return RegionOfInterest{50.0 - size.width * 0.5, 50.0 - size.height * 0.5, 50.0 + size.width * 0.5,
                            50.0 + size.height * 0.5};
}

bool separatedFromWindow(const RegionOfInterest& region, double windowLeft, double windowTop, double windowRight,
                         double windowBottom)
{
    return region.rightPercent + StarterGap <= windowLeft || region.leftPercent >= windowRight + StarterGap ||
           region.bottomPercent + StarterGap <= windowTop || region.topPercent >= windowBottom + StarterGap;
}

double starterAreaScore(const StarterArea& area, const StarterSize& size)
{
    const double availableWidth = std::max(0.0, area.right - area.left);
    const double availableHeight = std::max(0.0, area.bottom - area.top);
    const double width = std::min(size.width, availableWidth);
    const double height = std::min(size.height, availableHeight);
    const bool horizontal = area.side == StarterSide::Left || area.side == StarterSide::Right;
    const double fit = std::min(width / size.width, height / size.height);

    return fit * 1000.0 + (horizontal ? 10.0 : 0.0) + availableWidth * availableHeight / 10000.0;
}

RegionOfInterest starterRegionInArea(const StarterArea& area, const StarterSize& size, double windowCentreX,
                                     double windowCentreY)
{
    const double widthScale = std::max(0.0, area.right - area.left) / size.width;
    const double heightScale = std::max(0.0, area.bottom - area.top) / size.height;
    const double scale = std::min({1.0, widthScale, heightScale});
    const double width = size.width * scale;
    const double height = size.height * scale;
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

    return RegionOfInterest{left, top, left + width, top + height};
}

RegionOfInterest openStarterRegion(const StarterSize& size, double windowLeft, double windowTop, double windowRight,
                                   double windowBottom)
{
    const double limit = 100.0 - StarterMargin;
    const std::array<StarterArea, 4> areas{
        StarterArea{StarterSide::Left, StarterMargin, StarterMargin, std::max(StarterMargin, windowLeft - StarterGap),
                    limit},
        StarterArea{StarterSide::Right, std::min(limit, windowRight + StarterGap), StarterMargin, limit, limit},
        StarterArea{StarterSide::Above, StarterMargin, StarterMargin, limit,
                    std::max(StarterMargin, windowTop - StarterGap)},
        StarterArea{StarterSide::Below, StarterMargin, std::min(limit, windowBottom + StarterGap), limit, limit},
    };
    const double centreX = (windowLeft + windowRight) * 0.5;
    const double centreY = (windowTop + windowBottom) * 0.5;
    RegionOfInterest best = centeredStarterRegion(size);
    double bestScore = -std::numeric_limits<double>::infinity();
    for (const StarterArea& area : areas) {
        const double score = starterAreaScore(area, size);
        if (score > bestScore) {
            best = starterRegionInArea(area, size, centreX, centreY);
            bestScore = score;
        }
    }

    return best;
}

}  // namespace

WindowPlacement starterWindowPlacement(const WindowPlacement& workArea, int windowWidth, int windowHeight)
{
    const int width = std::clamp(windowWidth, 1, std::max(1, workArea.width));
    const int height = std::clamp(windowHeight, 1, std::max(1, workArea.height));
    const int inset = static_cast<int>(static_cast<double>(std::max(0, workArea.width)) * StarterWindowInset);
    const auto coordinate = [](long long value) {
        return static_cast<int>(std::clamp(value, static_cast<long long>(std::numeric_limits<int>::min()),
                                           static_cast<long long>(std::numeric_limits<int>::max())));
    };
    const int x = coordinate(static_cast<long long>(workArea.x) + std::min(inset, std::max(0, workArea.width - width)));
    const int y = coordinate(static_cast<long long>(workArea.y) + std::max(0, workArea.height - height) / 2);

    return WindowPlacement{x, y, width, height};
}

std::optional<uint32_t> displayUnderWindow(const WindowPlacement& window)
{
    return displayAtPoint(DesktopPoint{window.x + window.width / 2.0, window.y + window.height / 2.0});
}

RegionOfInterest starterGlobalRegion(const WindowPlacement& window, const DisplayGeometry& display)
{
    const StarterSize size = squareStarterSize(display);
    const RegionOfInterest centered = centeredStarterRegion(size);
    if (display.widthPoints <= 0.0 || display.heightPoints <= 0.0) {
        return centered;
    }

    const double windowLeft = std::clamp(percent(window.x, display.originX, display.widthPoints), 0.0, 100.0);
    const double windowTop = std::clamp(percent(window.y, display.originY, display.heightPoints), 0.0, 100.0);
    const double windowRight = std::clamp(
        percent(static_cast<double>(window.x) + window.width, display.originX, display.widthPoints), 0.0, 100.0);
    const double windowBottom = std::clamp(
        percent(static_cast<double>(window.y) + window.height, display.originY, display.heightPoints), 0.0, 100.0);
    if (separatedFromWindow(centered, windowLeft, windowTop, windowRight, windowBottom)) {
        return centered;
    }

    return openStarterRegion(size, windowLeft, windowTop, windowRight, windowBottom);
}

IntRect selfWindowMask(const WindowPlacement& window, const DisplayGeometry& display, int displayWidth,
                       int displayHeight, float uiScale)
{
    if (!(display.widthPoints > 0.0) || !(display.heightPoints > 0.0) || displayWidth <= 0 || displayHeight <= 0 ||
        !std::isfinite(display.originX) || !std::isfinite(display.originY) || !std::isfinite(uiScale)) {
        return {};
    }
    const double scaleX = displayWidth / display.widthPoints;
    const double scaleY = displayHeight / display.heightPoints;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY)) {
        return {};
    }
    const double x = (window.x - display.originX - MarginLeft * uiScale) * scaleX;
    const double y = (window.y - display.originY - MarginTop * uiScale) * scaleY;
    const double xEnd =
        (static_cast<double>(window.x) + window.width - display.originX + (MarginWidth - MarginLeft) * uiScale) *
        scaleX;
    const double yEnd =
        (static_cast<double>(window.y) + window.height - display.originY + (MarginHeight - MarginTop) * uiScale) *
        scaleY;
    // Only the overlap can mask captured pixels. Clip before narrowing to int:
    // desktop coordinates on another monitor can lie beyond its pixel range.
    const int left = static_cast<int>(std::clamp(std::floor(x), 0.0, static_cast<double>(displayWidth)));
    const int top = static_cast<int>(std::clamp(std::floor(y), 0.0, static_cast<double>(displayHeight)));
    const int right = static_cast<int>(std::clamp(std::ceil(xEnd), 0.0, static_cast<double>(displayWidth)));
    const int bottom = static_cast<int>(std::clamp(std::ceil(yEnd), 0.0, static_cast<double>(displayHeight)));

    return IntRect{left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

}  // namespace sidescopes
