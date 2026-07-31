#pragma once

#include <algorithm>
#include <optional>

#include "core/frame.h"
#include "platform/desktop.h"

namespace sidescopes {

/// Where to read from the root and how to stamp the result: the root-relative
/// rectangle to grab, and the source origin and display extents a frame
/// records so a region resolves against the whole display, not the crop.
///
/// Pure geometry, no Xlib, so the crop-and-stamp arithmetic - which has twice
/// been a shipped coordinate defect on other platforms - is unit-tested
/// without an X server.
struct GrabRect
{
    int rootX = 0;
    int rootY = 0;
    int width = 0;
    int height = 0;
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
};

/// The grab for a display, narrowed to @p crop when one is set. A whole-display
/// grab stamps all-zero source fields (the frame covers the display); a crop
/// stamps its origin with the display's extents, which is what a region
/// resolves against. The crop is in display pixels relative to the display's
/// own origin, and is clamped to the display so a stale region never asks the
/// server for pixels outside the root.
[[nodiscard]] inline GrabRect computeGrab(const DisplayGeometry& display, const std::optional<IntRect>& crop)
{
    const int displayWidth = static_cast<int>(display.widthPoints);
    const int displayHeight = static_cast<int>(display.heightPoints);
    const int originX = static_cast<int>(display.originX);
    const int originY = static_cast<int>(display.originY);
    if (!crop) {
        return GrabRect{originX, originY, displayWidth, displayHeight, 0, 0, 0, 0};
    }
    // The origin is clamped to leave at least one pixel of width and height, so
    // a crop that starts at or past the display edge still grabs a valid
    // rectangle rather than asking the server for a zero-size image. std::max
    // on the upper bound keeps std::clamp's lo <= hi even on a degenerate
    // zero-size display, which would otherwise be undefined.
    const int cropX = std::clamp(crop->x, 0, std::max(displayWidth - 1, 0));
    const int cropY = std::clamp(crop->y, 0, std::max(displayHeight - 1, 0));
    const int cropWidth = std::clamp(crop->width, 1, std::max(displayWidth - cropX, 1));
    const int cropHeight = std::clamp(crop->height, 1, std::max(displayHeight - cropY, 1));

    return GrabRect{originX + cropX, originY + cropY, cropWidth, cropHeight, cropX, cropY, displayWidth, displayHeight};
}

}  // namespace sidescopes
