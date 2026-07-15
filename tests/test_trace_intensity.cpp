#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/trace_intensity.h"

namespace sidescopes {

TEST_CASE("Trace intensity round-trips through gain")
{
    for (const float percent : {0.0f, 10.0f, 25.0f, 50.0f, 69.4f, 100.0f}) {
        CHECK(intensityFromTraceGain(traceGainFromIntensity(percent)) == Catch::Approx(percent).margin(0.01));
    }
}

TEST_CASE("Trace intensity hits the calibrated anchors")
{
    // Defaults calibrated against real editing sessions: waveform 0.05,
    // vectorscope 3.0. They must sit at comfortable slider positions.
    CHECK(intensityFromTraceGain(0.05f) == Catch::Approx(25.0).margin(0.1));
    CHECK(intensityFromTraceGain(3.0f, VectorscopeIntensityShift) == Catch::Approx(49.4).margin(0.2));
}

TEST_CASE("Shifted scale reaches gains beyond the unshifted ceiling")
{
    // The vectorscope's 100% must sit above the shared scale's maximum:
    // the whole point of the shift is headroom for dense photographs.
    CHECK(traceGainFromIntensity(100.0f, VectorscopeIntensityShift) > traceGainFromIntensity(100.0f));
    // Round trip holds on the shifted scale too.
    for (const float percent : {0.0f, 49.4f, 100.0f}) {
        CHECK(intensityFromTraceGain(traceGainFromIntensity(percent, VectorscopeIntensityShift),
                                     VectorscopeIntensityShift) == Catch::Approx(percent).margin(0.01));
    }
}

TEST_CASE("Trace intensity is clamped and monotonic")
{
    CHECK(traceGainFromIntensity(-10.0f) == traceGainFromIntensity(0.0f));
    CHECK(traceGainFromIntensity(150.0f) == traceGainFromIntensity(100.0f));

    float previous = 0.0f;
    for (int percent = 0; percent <= 100; percent += 5) {
        const float gain = traceGainFromIntensity(static_cast<float>(percent));
        CHECK(gain > previous);
        previous = gain;
    }
}

}  // namespace sidescopes
