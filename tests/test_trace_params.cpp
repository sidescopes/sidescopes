#include <catch2/catch_test_macros.hpp>

#include "app/scope_registry.h"
#include "app/trace_params.h"

namespace sidescopes {

TEST_CASE("An unadjusted trace reads zero")
{
    // Zero is the "nothing stored" answer; the app seeds both controls from the
    // preferences at startup.
    const TraceParams traces;
    CHECK(traces.intensity(VectorscopeScopeId) == 0.0f);
    CHECK(traces.smoothing(VectorscopeScopeId) == 0.0f);
}

TEST_CASE("Intensity and smoothing are tracked per trace")
{
    TraceParams traces;
    traces.setIntensity(VectorscopeScopeId, 40.0f);
    traces.setIntensity(WaveformScopeId, 60.0f);
    CHECK(traces.intensity(VectorscopeScopeId) == 40.0f);
    CHECK(traces.intensity(WaveformScopeId) == 60.0f);

    traces.setSmoothing(VectorscopeScopeId, 120.0f);
    traces.setSmoothing(WaveformScopeId, 250.0f);
    CHECK(traces.smoothing(VectorscopeScopeId) == 120.0f);
    CHECK(traces.smoothing(WaveformScopeId) == 250.0f);
}

TEST_CASE("The parade shares the waveform's trace controls")
{
    TraceParams traces;
    traces.setIntensity(WaveformScopeId, 60.0f);
    traces.setSmoothing(WaveformScopeId, 250.0f);
    CHECK(traces.intensity(ParadeScopeId) == 60.0f);
    CHECK(traces.smoothing(ParadeScopeId) == 250.0f);

    // The alias runs both ways: adjusting the parade moves the waveform.
    traces.setIntensity(ParadeScopeId, 70.0f);
    traces.setSmoothing(ParadeScopeId, 300.0f);
    CHECK(traces.intensity(WaveformScopeId) == 70.0f);
    CHECK(traces.smoothing(WaveformScopeId) == 300.0f);
    // Every other scope keeps its own control.
    CHECK(traces.intensity(VectorscopeScopeId) == 0.0f);
}

}  // namespace sidescopes
