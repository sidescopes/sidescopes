#include "app/region_geometry.h"

namespace sidescopes {

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
