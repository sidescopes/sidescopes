#include "app/trace_params.h"

#include "app/scope_registry.h"

namespace sidescopes {

std::string_view traceControlOwner(std::string_view id)
{
    return id == ParadeScopeId ? std::string_view{WaveformScopeId} : id;
}

float TraceParams::intensity(std::string_view id) const
{
    const auto at = m_intensity.find(traceControlOwner(id));

    return at != m_intensity.end() ? at->second : 0.0f;
}

void TraceParams::setIntensity(std::string_view id, float percent)
{
    m_intensity[std::string{traceControlOwner(id)}] = percent;
}

float TraceParams::smoothing(std::string_view id) const
{
    const auto at = m_smoothing.find(traceControlOwner(id));

    return at != m_smoothing.end() ? at->second : 0.0f;
}

void TraceParams::setSmoothing(std::string_view id, float milliseconds)
{
    m_smoothing[std::string{traceControlOwner(id)}] = milliseconds;
}

}  // namespace sidescopes
