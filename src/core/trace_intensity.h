#pragma once

namespace sidescopes {

// Users adjust traces through a 0-100% "intensity" control rather than a raw
// gain number. The log density mapping in the engines responds
// logarithmically to gain, so a linear gain slider feels wildly uneven; the
// intensity control is exponential in the underlying gain, which makes every
// slider step and scroll notch change the trace by a perceptually similar
// amount. 0% maps to a near-linear, dim trace and 100% to a hot one.
//
// `shift_percent` slides a scope's whole scale along the gain curve:
// the vectorscope runs 20 points hotter, because dense photographs kept
// asking for more than the shared scale's ceiling while its lower fifth
// went unused.
//
// The calibrated defaults land at comfortable positions on this scale:
// a waveform gain of 0.05 is 25%, a vectorscope gain of 3.0 is about
// 49% on its shifted scale.
float TraceGainFromIntensity(float intensity_percent, float shift_percent = 0.0f);
float IntensityFromTraceGain(float gain, float shift_percent = 0.0f);

// The vectorscope's scale shift, shared by every conversion site.
constexpr float kVectorscopeIntensityShift = 20.0f;

}  // namespace sidescopes
