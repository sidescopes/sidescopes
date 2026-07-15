#include "core/marker_smoother.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

FloatColor averageNeighborhood(const FrameView& frame, int px, int py, int radius)
{
    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
    int count = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int sampleX = px + dx;
            const int sampleY = py + dy;
            if (sampleX < 0 || sampleX >= frame.width || sampleY < 0 || sampleY >= frame.height) {
                continue;
            }
            const Color color = frame.colorAt(sampleX, sampleY);
            sumR += static_cast<float>(color.r);
            sumG += static_cast<float>(color.g);
            sumB += static_cast<float>(color.b);
            ++count;
        }
    }
    if (count == 0) {
        return FloatColor{};
    }
    const float scale = 1.0f / static_cast<float>(count);
    return FloatColor{sumR * scale, sumG * scale, sumB * scale};
}

FloatColor MarkerSmoother::update(const FloatColor& target, float elapsedSeconds)
{
    const float tauSeconds = m_timeConstantMs / 1000.0f;
    const float alpha = tauSeconds <= 0.0f ? 1.0f : 1.0f - std::exp(-elapsedSeconds / tauSeconds);

    m_value.r += alpha * (target.r - m_value.r);
    m_value.g += alpha * (target.g - m_value.g);
    m_value.b += alpha * (target.b - m_value.b);

    const bool nearTarget = std::fabs(target.r - m_value.r) < SnapWindow &&
                            std::fabs(target.g - m_value.g) < SnapWindow &&
                            std::fabs(target.b - m_value.b) < SnapWindow;
    if (nearTarget) {
        m_value = target;
    }
    return m_value;
}

}  // namespace sidescopes
