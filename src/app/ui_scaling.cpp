#include "app/ui_scaling.h"

namespace sidescopes {

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
