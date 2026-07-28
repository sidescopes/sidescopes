#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace sidescopes {

/// @brief Each trace's intensity and marker smoothing.
///
/// The host's own controls over how a trace is drawn, distinct from the module
/// parameters the worker reads: intensity seeds a scope's gain, and smoothing
/// paces its cursor markers.
/// The scope whose trace controls @p id uses: the parade shares the waveform's
/// intensity and smoothing, because they are one trace shown two ways and one
/// control is what the panes have always offered. Every other scope owns its
/// own - the luma waveform included, since it is a second instrument rather
/// than a second view of the first.
[[nodiscard]] std::string_view traceControlOwner(std::string_view id);

class TraceParams
{
public:
    /// Trace intensity and marker smoothing are tracked per control-owner id;
    /// the parade shares the waveform's, resolved here.
    [[nodiscard]] float intensity(std::string_view id) const;
    void setIntensity(std::string_view id, float percent);

    [[nodiscard]] float smoothing(std::string_view id) const;
    void setSmoothing(std::string_view id, float milliseconds);

private:
    std::map<std::string, float, std::less<>> m_intensity;
    std::map<std::string, float, std::less<>> m_smoothing;
};

}  // namespace sidescopes
