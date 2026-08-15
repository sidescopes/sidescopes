#include "web/band_geometry.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

bool BandRect::empty() const
{
    return right <= left || bottom <= top;
}

float BandRect::area() const
{
    return empty() ? 0.0f : (right - left) * (bottom - top);
}

std::array<BandRect, 4> bandQuarters(const BandRect& outer, const BandRect& hole)
{
    const float left = std::round(outer.left);
    const float top = std::round(outer.top);
    const float right = std::round(outer.right);
    const float bottom = std::round(outer.bottom);
    // Held inside the outer rectangle, so a hole larger than the band cannot
    // turn a quarter inside out. That happens for real: the band is clipped to
    // the picture, and a region pushed against an edge leaves less room
    // outside it than the band wants.
    const float holeLeft = std::clamp(std::round(hole.left), left, right);
    const float holeTop = std::clamp(std::round(hole.top), top, bottom);
    const float holeRight = std::clamp(std::round(hole.right), holeLeft, right);
    const float holeBottom = std::clamp(std::round(hole.bottom), holeTop, bottom);

    // Top and bottom take the full width; the sides take what is left between
    // them. Any other division would have the corners belong to two quarters.
    return {BandRect{left, top, right, holeTop}, BandRect{left, holeBottom, right, bottom},
            BandRect{left, holeTop, holeLeft, holeBottom}, BandRect{holeRight, holeTop, right, holeBottom}};
}

}  // namespace sidescopes
