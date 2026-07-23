#include "app/ui_scaling.h"

#include <cmath>

namespace sidescopes {

float cleanedUiScaleFactor(float requested)
{
    // Zero, negative, and NaN are not real scales; fall back to Default rather
    // than to the smallest step. (NaN fails every comparison, so this guard is
    // what catches it - snapping alone would keep the seed.)
    if (!(requested > 0.0f)) {
        return 1.0f;
    }
    // Seed with Default so a value far from every step lands there, not on the
    // nearest end of the range.
    float best = 1.0f;
    float bestDelta = std::fabs(requested - best);
    for (const float step : UiScaleSteps) {
        const float delta = std::fabs(requested - step);
        if (delta < bestDelta) {
            best = step;
            bestDelta = delta;
        }
    }

    return best;
}

float interfaceFontDensity(int windowWidthPx, int framebufferWidthPx)
{
    if (windowWidthPx <= 0 || framebufferWidthPx <= windowWidthPx) {
        return 1.0f;
    }

    return static_cast<float>(framebufferWidthPx) / static_cast<float>(windowWidthPx);
}

float uiScaleForWindow(float contentScale, int windowWidthPx, int framebufferWidthPx)
{
    if (windowWidthPx <= 0) {
        return contentScale;
    }
    const float framebufferRatio = static_cast<float>(framebufferWidthPx) / static_cast<float>(windowWidthPx);

    return framebufferRatio > 0.0f ? contentScale / framebufferRatio : contentScale;
}

}  // namespace sidescopes
