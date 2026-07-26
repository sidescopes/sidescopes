#include "app/region_geometry.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

double regionTravelPercent(const std::optional<RegionOfInterest>& from, const std::optional<RegionOfInterest>& to)
{
    if (!from || !to) {
        return 0.0;
    }

    return std::max({std::abs(to->leftPercent - from->leftPercent), std::abs(to->rightPercent - from->rightPercent),
                     std::abs(to->topPercent - from->topPercent), std::abs(to->bottomPercent - from->bottomPercent)});
}

LockRect lockRectFromPercent(const RegionOfInterest& region, int frameWidth, int frameHeight)
{
    return LockRect{region.leftPercent / 100.0 * frameWidth, region.topPercent / 100.0 * frameHeight,
                    region.rightPercent / 100.0 * frameWidth, region.bottomPercent / 100.0 * frameHeight};
}

RegionOfInterest percentFromLockRect(const LockRect& rect, int frameWidth, int frameHeight)
{
    return RegionOfInterest{rect.left * 100.0 / frameWidth, rect.top * 100.0 / frameHeight,
                            rect.right * 100.0 / frameWidth, rect.bottom * 100.0 / frameHeight};
}

}  // namespace sidescopes
