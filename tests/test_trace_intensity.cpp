#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/trace_intensity.h"

namespace sidescopes {

TEST_CASE("Trace intensity round-trips through gain") {
    for (const float percent : {0.0f, 10.0f, 25.0f, 50.0f, 69.4f, 100.0f}) {
        CHECK(IntensityFromTraceGain(TraceGainFromIntensity(percent)) ==
              Catch::Approx(percent).margin(0.01));
    }
}

TEST_CASE("Trace intensity hits the calibrated anchors") {
    // Defaults calibrated against real editing sessions: waveform 0.05,
    // vectorscope 3.0. They must sit at comfortable slider positions.
    CHECK(IntensityFromTraceGain(0.05f) == Catch::Approx(25.0).margin(0.1));
    CHECK(IntensityFromTraceGain(3.0f, kVectorscopeIntensityShift) ==
          Catch::Approx(49.4).margin(0.2));
}

TEST_CASE("Shifted scale reaches gains beyond the unshifted ceiling") {
    // The vectorscope's 100% must sit above the shared scale's maximum:
    // the whole point of the shift is headroom for dense photographs.
    CHECK(TraceGainFromIntensity(100.0f, kVectorscopeIntensityShift) >
          TraceGainFromIntensity(100.0f));
    // Round trip holds on the shifted scale too.
    for (const float percent : {0.0f, 49.4f, 100.0f}) {
        CHECK(IntensityFromTraceGain(TraceGainFromIntensity(percent, kVectorscopeIntensityShift),
                                     kVectorscopeIntensityShift) ==
              Catch::Approx(percent).margin(0.01));
    }
}

TEST_CASE("Trace intensity is clamped and monotonic") {
    CHECK(TraceGainFromIntensity(-10.0f) == TraceGainFromIntensity(0.0f));
    CHECK(TraceGainFromIntensity(150.0f) == TraceGainFromIntensity(100.0f));

    float previous = 0.0f;
    for (int percent = 0; percent <= 100; percent += 5) {
        const float gain = TraceGainFromIntensity(static_cast<float>(percent));
        CHECK(gain > previous);
        previous = gain;
    }
}

}  // namespace sidescopes
