// The region picker's spatial math, kept free of any windowing toolkit so
// it can be reasoned about and unit-tested on its own. The GDI+ overlay
// and the Win32 border procedure adapt their own rectangle types to
// LocalRect at the boundary.

#include "platform/windows/region_geometry.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

// Within this distance of a region corner, a grab resizes both axes.
constexpr double CornerZone = 22.0;
// Half-length of the edge-midpoint handle's grab zone along its edge.
constexpr double MidpointZone = 22.0;

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
    const double corner = CornerZone * scale;
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
    const double midpoint = MidpointZone * scale;
    const bool midX = std::abs(x - (region.x + region.width / 2)) <= midpoint;
    const bool midY = std::abs(y - (region.y + region.height / 2)) <= midpoint;
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

}  // namespace sidescopes
