#pragma once

#include <optional>

#include "core/frame.h"

namespace sidescopes {

/// Average color of the (2*radius+1)^2 neighborhood around a pixel, clipped
/// to the frame. A single raw pixel flips wildly over text and fine detail;
/// a small neighborhood is the first half of making the cursor marker calm.
[[nodiscard]] FloatColor averageNeighborhood(const FrameView& frame, int px, int py, int radius = 1);

/// Exponential smoothing with a snap window — the second half. The smoothed
/// value stays in floating point end to end (quantizing it makes the marker
/// dither between adjacent scope bins while settling), and once every channel
/// is within the snap window the value locks onto the target, ending the
/// asymptotic tail decisively instead of letting it hover.
class MarkerSmoother
{
public:
    /// A time constant of zero disables smoothing entirely.
    void setTimeConstant(float milliseconds)
    {
        m_timeConstantMs = milliseconds;
    }

    FloatColor update(const FloatColor& target, float elapsedSeconds);

    /// Forgets what it was smoothing, so the next update lands on its target
    /// rather than easing towards it from where the last one left off. What a
    /// marker that stopped being drawn needs: it comes back at the colour under
    /// the pointer instead of sweeping across the trace from the colour it left
    /// on.
    void forget()
    {
        m_value.reset();
    }

private:
    static constexpr float SnapWindow = 0.75f;

    float m_timeConstantMs = 100.0f;
    /// Empty only after forget(): the value eased from, mid-gray until the
    /// first marker has anywhere else to come from.
    std::optional<FloatColor> m_value{FloatColor{128.0f, 128.0f, 128.0f}};
};

}  // namespace sidescopes
