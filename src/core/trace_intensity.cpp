#include "core/trace_intensity.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

// Gain spans four decades: 0.005 at 0% up to 50 at 100%.
constexpr float MinimumGain = 0.005f;
constexpr float Decades = 4.0f;

}  // namespace

float traceGainFromIntensity(float intensityPercent, float shiftPercent)
{
    const float clamped = std::clamp(intensityPercent, 0.0f, 100.0f);
    return MinimumGain * std::pow(10.0f, (clamped + shiftPercent) * Decades / 100.0f);
}

float intensityFromTraceGain(float gain, float shiftPercent)
{
    if (gain <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(100.0f * std::log10(gain / MinimumGain) / Decades - shiftPercent, 0.0f, 100.0f);
}

}  // namespace sidescopes
