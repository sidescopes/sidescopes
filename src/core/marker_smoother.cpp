#include "core/marker_smoother.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

FloatColor AverageNeighborhood(const FrameView& frame, int px, int py, int radius) {
    float sum_r = 0.0f, sum_g = 0.0f, sum_b = 0.0f;
    int count = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int sample_x = px + dx;
            const int sample_y = py + dy;
            if (sample_x < 0 || sample_x >= frame.width || sample_y < 0 || sample_y >= frame.height)
                continue;
            const Color color = frame.ColorAt(sample_x, sample_y);
            sum_r += static_cast<float>(color.r);
            sum_g += static_cast<float>(color.g);
            sum_b += static_cast<float>(color.b);
            ++count;
        }
    }
    if (count == 0) return FloatColor{};
    const float scale = 1.0f / static_cast<float>(count);
    return FloatColor{sum_r * scale, sum_g * scale, sum_b * scale};
}

FloatColor MarkerSmoother::Update(const FloatColor& target, float elapsed_seconds) {
    const float tau_seconds = time_constant_ms_ / 1000.0f;
    const float alpha =
        tau_seconds <= 0.0f ? 1.0f : 1.0f - std::exp(-elapsed_seconds / tau_seconds);

    value_.r += alpha * (target.r - value_.r);
    value_.g += alpha * (target.g - value_.g);
    value_.b += alpha * (target.b - value_.b);

    const bool near_target = std::fabs(target.r - value_.r) < kSnapWindow &&
                             std::fabs(target.g - value_.g) < kSnapWindow &&
                             std::fabs(target.b - value_.b) < kSnapWindow;
    if (near_target) value_ = target;
    return value_;
}

}  // namespace sidescopes
