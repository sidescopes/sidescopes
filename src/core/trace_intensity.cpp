#include "core/trace_intensity.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

// Gain spans four decades: 0.005 at 0% up to 50 at 100%.
constexpr float kMinimumGain = 0.005f;
constexpr float kDecades = 4.0f;

}  // namespace

float TraceGainFromIntensity(float intensity_percent, float shift_percent) {
    const float clamped = std::clamp(intensity_percent, 0.0f, 100.0f);
    return kMinimumGain * std::pow(10.0f, (clamped + shift_percent) * kDecades / 100.0f);
}

float IntensityFromTraceGain(float gain, float shift_percent) {
    if (gain <= 0.0f) return 0.0f;
    return std::clamp(100.0f * std::log10(gain / kMinimumGain) / kDecades - shift_percent, 0.0f,
                      100.0f);
}

}  // namespace sidescopes
