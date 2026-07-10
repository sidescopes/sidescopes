#pragma once

namespace sidescopes {

// Users adjust traces through a 0-100% "intensity" control rather than a raw
// gain number. The log density mapping in the engines responds
// logarithmically to gain, so a linear gain slider feels wildly uneven; the
// intensity control is exponential in the underlying gain, which makes every
// slider step and scroll notch change the trace by a perceptually similar
// amount. 0% maps to a near-linear, dim trace and 100% to a hot one.
//
// The calibrated defaults land at comfortable positions on this scale:
// a waveform gain of 0.05 is 25%, a vectorscope gain of 3.0 is about 69%.
float TraceGainFromIntensity(float intensity_percent);
float IntensityFromTraceGain(float gain);

}  // namespace sidescopes
