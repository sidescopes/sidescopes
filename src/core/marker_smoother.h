#pragma once

#include <optional>

#include "core/frame.h"

namespace sidescopes {

// Average color of the (2*radius+1)^2 neighborhood around a pixel, clipped
// to the frame. A single raw pixel flips wildly over text and fine detail;
// a small neighborhood is the first half of making the cursor marker calm.
FloatColor AverageNeighborhood(const FrameView& frame, int px, int py, int radius = 1);

// Exponential smoothing with a snap window — the second half. The smoothed
// value stays in floating point end to end (quantizing it makes the marker
// dither between adjacent scope bins while settling), and once every channel
// is within the snap window the value locks onto the target, ending the
// asymptotic tail decisively instead of letting it hover.
class MarkerSmoother {
public:
    // A time constant of zero disables smoothing entirely.
    void SetTimeConstant(float milliseconds) { time_constant_ms_ = milliseconds; }

    FloatColor Update(const FloatColor& target, float elapsed_seconds);

private:
    static constexpr float kSnapWindow = 0.75f;

    float time_constant_ms_ = 100.0f;
    FloatColor value_{128.0f, 128.0f, 128.0f};
};

}  // namespace sidescopes
