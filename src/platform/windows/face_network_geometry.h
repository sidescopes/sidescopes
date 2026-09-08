#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace sidescopes::face_network {

/// YuNet downsamples by 32. Keep its coarsest convolution input at least 2x2
/// to avoid OpenCV's depthwise padding bug on singleton spatial dimensions.
[[nodiscard]] inline int paddedInputEdge(int edge)
{
    return std::max(64, (edge + 31) / 32 * 32);
}

/// Validates decoded network geometry before integer NMS. OpenCV adds two
/// integer rectangle areas when computing overlap, before converting to double.
[[nodiscard]] inline bool validNmsBounds(double x, double y, double width, double height)
{
    constexpr double CoordinateLimit = 1000000.0;
    for (double value : {x, y, width, height}) {
        if (!std::isfinite(value) || std::abs(value) > CoordinateLimit) {
            return false;
        }
    }
    constexpr int MaximumArea = std::numeric_limits<int>::max() / 2;
    return width > 0.0 && height > 0.0 && width * height <= MaximumArea;
}

}  // namespace sidescopes::face_network
