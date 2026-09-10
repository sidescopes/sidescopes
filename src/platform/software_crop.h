#pragma once

#include <optional>

#include "core/frame.h"

namespace sidescopes {

/// The part of a display a backend that narrows in software copies out of a
/// whole-display frame: @p crop clamped to the display, or the whole display
/// when nothing is asked for or the request leaves nothing inside it. A frame
/// copied from the result is stamped with the rectangle's origin and the
/// display's extents, so the region resolves to the same pixels either way.
[[nodiscard]] inline IntRect softwareCropRect(const std::optional<IntRect>& crop, int displayWidth, int displayHeight)
{
    const IntRect whole{0, 0, displayWidth, displayHeight};
    if (!crop) {
        return whole;
    }
    const IntRect clamped = crop->clampedTo(displayWidth, displayHeight);

    return clamped.empty() ? whole : clamped;
}

}  // namespace sidescopes
