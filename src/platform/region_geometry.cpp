// The region picker's spatial math, kept free of any windowing toolkit so
// it can be reasoned about and unit-tested on its own. Each platform's
// overlay and border code adapts its own rectangle types to LocalRect at
// the boundary.

#include "platform/region_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace sidescopes {
namespace {

// Within this distance of a region corner, a grab resizes both axes.
constexpr double CornerZone = 22.0;
// Half-length of the edge-midpoint handle's grab zone along its edge.
constexpr double MidpointZone = 22.0;

double rectArea(const LocalRect& rect)
{
    return std::max(0.0, rect.width) * std::max(0.0, rect.height);
}

// Half-open on the right and bottom edges, so abutting rectangles neither
// double-count a shared border nor let a zero-area rectangle contain anything.
bool containsPoint(const LocalRect& rect, double x, double y)
{
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

bool pointCovered(const std::vector<LocalRect>& occluders, double x, double y)
{
    for (const LocalRect& occluder : occluders) {
        if (containsPoint(occluder, x, y)) {
            return true;
        }
    }

    return false;
}

// The area of `target` left uncovered by the union of `occluders`. The
// coordinate grid is compressed to the target's bounds: every cell there lies
// wholly inside or outside each rectangle, so its center decides the whole
// cell. Exact for axis-aligned rectangles. `target` must have positive area.
double unoccludedArea(const LocalRect& target, const std::vector<LocalRect>& occluders)
{
    const double right = target.x + target.width;
    const double bottom = target.y + target.height;
    std::vector<double> xs{target.x, right};
    std::vector<double> ys{target.y, bottom};
    for (const LocalRect& occluder : occluders) {
        xs.push_back(std::clamp(occluder.x, target.x, right));
        xs.push_back(std::clamp(occluder.x + occluder.width, target.x, right));
        ys.push_back(std::clamp(occluder.y, target.y, bottom));
        ys.push_back(std::clamp(occluder.y + occluder.height, target.y, bottom));
    }

    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    double visible = 0.0;
    for (std::size_t xi = 0; xi + 1 < xs.size(); ++xi) {
        for (std::size_t yi = 0; yi + 1 < ys.size(); ++yi) {
            const double cellWidth = xs[xi + 1] - xs[xi];
            const double cellHeight = ys[yi + 1] - ys[yi];
            if (cellWidth <= 0.0 || cellHeight <= 0.0) {
                continue;
            }

            const double centerX = (xs[xi] + xs[xi + 1]) / 2.0;
            const double centerY = (ys[yi] + ys[yi + 1]) / 2.0;
            if (!pointCovered(occluders, centerX, centerY)) {
                visible += cellWidth * cellHeight;
            }
        }
    }

    return visible;
}

}  // namespace

RegionOfInterest regionFromLocalRect(const LocalRect& rect, double width, double height)
{
    RegionOfInterest region;
    region.leftPercent = rect.x / width * 100.0;
    region.topPercent = rect.y / height * 100.0;
    region.rightPercent = (rect.x + rect.width) / width * 100.0;
    region.bottomPercent = (rect.y + rect.height) / height * 100.0;
    return region;
}

LocalRect localRectFromRegion(const RegionOfInterest& region, double width, double height)
{
    return {region.leftPercent / 100.0 * width, region.topPercent / 100.0 * height,
            (region.rightPercent - region.leftPercent) / 100.0 * width,
            (region.bottomPercent - region.topPercent) / 100.0 * height};
}

LocalRect selectionRectFromDrag(double startX, double startY, double currentX, double currentY)
{
    return {std::min(startX, currentX), std::min(startY, currentY), std::abs(currentX - startX),
            std::abs(currentY - startY)};
}

unsigned cornerZoneAt(const LocalRect& region, double x, double y, double scale)
{
    // A small region must keep a generous move band - face-sized regions
    // are the product's bread and butter: the corner reach never eats more
    // than a sixth of either side, so resizing cannot crowd out moving.
    const double corner = std::min({CornerZone * scale, region.width / 6, region.height / 6});
    const bool nearLeft = x < region.x + corner;
    const bool nearRight = x > region.x + region.width - corner;
    const bool nearTop = y < region.y + corner;
    const bool nearBottom = y > region.y + region.height - corner;
    if ((nearLeft || nearRight) && (nearTop || nearBottom)) {
        unsigned zone = ZoneNone;
        zone |= nearLeft ? ZoneLeft : ZoneRight;
        zone |= nearTop ? ZoneTop : ZoneBottom;
        return zone;
    }
    return ZoneNone;
}

unsigned edgeOrMoveZoneAt(const LocalRect& region, double x, double y, double scale)
{
    // The same courtesy as the corners: an edge-midpoint zone never spans
    // more than a sixth of its edge, leaving roughly half of every edge as
    // move band however small the region gets.
    const double midpointX = std::min(MidpointZone * scale, region.width / 12);
    const double midpointY = std::min(MidpointZone * scale, region.height / 12);
    const bool midX = std::abs(x - (region.x + region.width / 2)) <= midpointX;
    const bool midY = std::abs(y - (region.y + region.height / 2)) <= midpointY;
    if (midX && y < region.y) {
        return ZoneTop;
    }
    if (midX && y > region.y + region.height) {
        return ZoneBottom;
    }
    if (midY && x < region.x) {
        return ZoneLeft;
    }
    if (midY && x > region.x + region.width) {
        return ZoneRight;
    }
    return ZoneMove;
}

LocalRect draggedRegionRect(unsigned dragZone, const LocalRect& start, double dx, double dy, double minimum)
{
    const double startLeft = start.x;
    const double startTop = start.y;
    const double startRight = start.x + start.width;
    const double startBottom = start.y + start.height;
    double left = startLeft;
    double top = startTop;
    double right = startRight;
    double bottom = startBottom;
    if ((dragZone & ZoneMove) != 0) {
        left += dx;
        right += dx;
        top += dy;
        bottom += dy;
    } else {
        if ((dragZone & ZoneLeft) != 0) {
            left = std::min(startLeft + dx, startRight - minimum);
        }
        if ((dragZone & ZoneRight) != 0) {
            right = std::max(startRight + dx, startLeft + minimum);
        }
        if ((dragZone & ZoneTop) != 0) {
            top = std::min(startTop + dy, startBottom - minimum);
        }
        if ((dragZone & ZoneBottom) != 0) {
            bottom = std::max(startBottom + dy, startTop + minimum);
        }
    }
    return {left, top, right - left, bottom - top};
}

std::vector<double> visibleFractions(const std::vector<LocalRect>& windows)
{
    std::vector<double> fractions(windows.size(), 0.0);
    std::vector<LocalRect> occludersAbove;
    occludersAbove.reserve(windows.size());
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const LocalRect& window = windows[index];
        const double area = rectArea(window);
        if (area > 0.0) {
            fractions[index] = unoccludedArea(window, occludersAbove) / area;
        }

        occludersAbove.push_back(window);
    }

    return fractions;
}

std::optional<std::size_t> topmostVisibleWindowAt(const std::vector<LocalRect>& windows, double x, double y)
{
    for (std::size_t index = 0; index < windows.size(); ++index) {
        if (containsPoint(windows[index], x, y)) {
            return index;
        }
    }

    return std::nullopt;
}

std::vector<std::size_t> meaningfulPickCandidates(const std::vector<LocalRect>& windows)
{
    const std::vector<double> fractions = visibleFractions(windows);
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < windows.size(); ++index) {
        if (fractions[index] >= MinimumVisibleFraction) {
            candidates.push_back(index);
        }
    }

    return candidates;
}

TabLayout borderTabLayout(double availableWidth, double attachZone, double textPad, double measuredText,
                          double minimumText)
{
    TabLayout layout;
    const double maximumText = availableWidth - attachZone - 2.0 * textPad;
    if (maximumText < minimumText) {
        return layout;  // a region too small for any legible label
    }
    layout.visible = true;
    layout.textWidth = std::min(measuredText, maximumText);
    layout.tabWidth = attachZone + layout.textWidth + 2.0 * textPad;

    return layout;
}

}  // namespace sidescopes
