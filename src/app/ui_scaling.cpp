#include "app/ui_scaling.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

constexpr float MillimetresPerInch = 25.4f;

/// The densest a real display gets. Above this the panel has misstated its
/// physical size - a stated width of a few millimetres reads as thousands of
/// units an inch - and the recommendation is worth nothing. Only the upper end
/// needs stating: a density under the reference already asks for nothing.
constexpr float GreatestPlausibleDensity = 600.0f;

}  // namespace

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

float recommendedUiScaleFactor(int modeWidth, int physicalWidthMm, float osUiScale)
{
    if (modeWidth <= 0 || physicalWidthMm <= 0 || !(osUiScale > 0.0f)) {
        return 1.0f;
    }
    const float inches = static_cast<float>(physicalWidthMm) / MillimetresPerInch;
    const float density = static_cast<float>(modeWidth) / inches / osUiScale;
    if (density > GreatestPlausibleDensity) {
        return 1.0f;
    }
    const float wanted = density / ReferenceUiDensity;
    if (wanted <= 1.0f) {
        return 1.0f;
    }

    return cleanedUiScaleFactor(std::min(wanted, MaximumAutomaticUiScaleFactor));
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
