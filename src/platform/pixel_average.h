#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "core/frame.h"

namespace sidescopes {

/// Averages @p count premultiplied RGBA pixels, weighting each by its alpha.
///
/// The weighting is not a refinement, it is the whole point. A one-shot screen
/// sample comes back letterboxed to its display's aspect - the pixels asked
/// for in a band across the middle, transparent padding above and below - so a
/// flat count over the buffer reports every colour at the fraction of it the
/// content covers. On a 16:9 display that is nine sixteenths: white read as
/// 56%.
///
/// @return The un-premultiplied average, or nothing when no pixel carried any
///         coverage at all.
[[nodiscard]] inline std::optional<FloatColor> averagePremultiplied(const uint8_t* rgba, std::size_t count)
{
    double sumRed = 0;
    double sumGreen = 0;
    double sumBlue = 0;
    double sumAlpha = 0;
    for (std::size_t index = 0; index < count; ++index) {
        sumRed += rgba[index * 4 + 0];
        sumGreen += rgba[index * 4 + 1];
        sumBlue += rgba[index * 4 + 2];
        sumAlpha += rgba[index * 4 + 3];
    }
    if (sumAlpha <= 0.0) {
        return std::nullopt;
    }
    // Dividing by the alpha total both un-premultiplies the channels and drops
    // the padding out of the count.
    const double covered = sumAlpha / 255.0;

    return FloatColor{static_cast<float>(sumRed / covered), static_cast<float>(sumGreen / covered),
                      static_cast<float>(sumBlue / covered)};
}

}  // namespace sidescopes
