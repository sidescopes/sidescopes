#include "app/trace_params.h"

#include "app/scope_registry.h"

namespace sidescopes {
namespace {

/// The control-owner id for @p id: the parade shares the waveform's intensity
/// and smoothing, so both resolve to one control.
std::string_view controlKey(std::string_view id)
{
    return id == ParadeScopeId ? std::string_view{WaveformScopeId} : id;
}

}  // namespace

float TraceParams::intensity(std::string_view id) const
{
    const auto at = m_intensity.find(controlKey(id));

    return at != m_intensity.end() ? at->second : 0.0f;
}

void TraceParams::setIntensity(std::string_view id, float percent)
{
    m_intensity[std::string{controlKey(id)}] = percent;
}

float TraceParams::smoothing(std::string_view id) const
{
    const auto at = m_smoothing.find(controlKey(id));

    return at != m_smoothing.end() ? at->second : 0.0f;
}

void TraceParams::setSmoothing(std::string_view id, float milliseconds)
{
    m_smoothing[std::string{controlKey(id)}] = milliseconds;
}

}  // namespace sidescopes
