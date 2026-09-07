#pragma once

#include <cmath>
#include <initializer_list>
#include <limits>

namespace sidescopes::face_network {

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
    return width > 0.0 && height > 0.0 && width * height <= std::numeric_limits<int>::max() / 2;
}

}  // namespace sidescopes::face_network
