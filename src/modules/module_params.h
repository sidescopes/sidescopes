#pragma once

#include <algorithm>
#include <cmath>

#include "core/trace_intensity.h"
#include "sidescopes/module.h"

namespace sidescopes {

/// Validate the complete batch before changing an instance. The boundary must
/// never narrow a non-finite value or a missing key into engine settings.
[[nodiscard]] inline bool validParameters(const SsParamValue* values, uint32_t count)
{
    if (count != 0 && values == nullptr) {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (values[index].key == nullptr || !std::isfinite(values[index].value)) {
            return false;
        }
    }
    return true;
}

/// Clamp before narrowing to the engine's float. The ceiling is the same one
/// the intensity control offers; zero remains available to direct callers.
[[nodiscard]] inline float parameterGain(double value, float shift = 0.0f)
{
    return static_cast<float>(std::clamp(value, 0.0, static_cast<double>(traceGainFromIntensity(100.0f, shift))));
}

}  // namespace sidescopes
