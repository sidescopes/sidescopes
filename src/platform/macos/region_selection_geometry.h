#pragma once

namespace sidescopes {

// macOS screens and views are bottom-left origin; the shared region
// geometry is top-left. Flip a Y coordinate within a container of the given
// height (whose top edge sits at that height).
inline double flippedY(double y, double height)
{
    return height - y;
}

}  // namespace sidescopes
