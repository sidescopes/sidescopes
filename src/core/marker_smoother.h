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
///
/// How fast it follows depends on how far it has to go — see Reach — so that a
/// reading taken somewhere new arrives promptly without the smoothing giving up
/// the jitter it exists to absorb.
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
    /// The distance, in 0-255 codes, at which a marker travels twice as fast as
    /// its time constant alone would carry it.
    ///
    /// Smoothing is there to absorb jitter, and jitter is small by nature: a
    /// noisy sample moves a reading by a fraction of a code. A large distance is
    /// not noise - it is a pointer put somewhere else deliberately, and the
    /// reading is wanted now. So the time constant is divided by
    /// 1 + distance/Reach. Close in that changes almost nothing; far out it
    /// bounds the time to arrive at timeConstant * ln(1 + Reach/SnapWindow)
    /// whatever the distance, where a fixed constant needs a further one for
    /// every e-fold. A marker crossing the whole range now arrives in the same
    /// time as one crossing a tenth of it.
    ///
    /// Six is measured, not chosen: at two, a pointer swept across a photograph
    /// starts to lurch again - a third of its frames off the average by half -
    /// while four, six and eight all keep the sweep even, and each further code
    /// costs settling time. It is one number because how a marker moves is a
    /// feel decision, and only the owner can grade it.
    static constexpr float Reach = 6.0f;

    float m_timeConstantMs = 100.0f;
    /// Empty only after forget(): the value eased from, mid-gray until the
    /// first marker has anywhere else to come from.
    std::optional<FloatColor> m_value{FloatColor{128.0f, 128.0f, 128.0f}};
};

}  // namespace sidescopes
