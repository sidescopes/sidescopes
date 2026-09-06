#pragma once

#include <optional>

namespace sidescopes {

/// How long the region must sit still after the user lets go before the scopes
/// go back to full detail. Longer than the gap between two steps of a drag, so
/// one pass is not computed at full detail in the middle of one, and short
/// enough that letting go is followed by the sharp trace.
inline constexpr double RegionSettleSeconds = 0.25;

/// What is moving the region, which is what decides what analysis does about
/// it. The two causes are opposites, and reading them as one state served
/// neither: a user moving the region is scanning a picture with it - over a
/// face, across a sky, hunting the part that is wrong - and wants the scopes
/// live under their hand, while a user moving the window an attached region
/// hangs off is rearranging a desktop and is reading nothing at all.
enum class RegionMotion
{
    /// Nothing is moving it.
    Still,
    /// The user is moving the region itself: a rubber band still being drawn,
    /// or the border dragged or resized by its band. A reading of somewhere in
    /// particular, taken while the somewhere is still being chosen.
    Dragged,
    /// An attached window the user is moving is carrying it. The region is in
    /// transit, not gone - which is why the last images stand rather than
    /// blanking, and why the hidden border is what says the reading is not
    /// live.
    Carried,
};

/// What one frame knows about the region's motion.
struct RegionMotionInputs
{
    /// The region differs from the one the worker was last told about.
    bool regionChanged = false;
    /// An attached window is being moved or resized by the user.
    bool windowMoving = false;
    double now = 0.0;
};

/// What one step decided.
struct RegionMotionStep
{
    RegionMotion motion = RegionMotion::Still;
    /// Whether analysis should stand while the region is in transit. Direct
    /// manipulation stays live under the active drag detail policy; only an
    /// attached window carrying the region holds it.
    bool holdAnalysis = false;
    /// Whether the motion just changed. The change itself has to be carried
    /// out: a region that stopped moving stops dirtying the settings, so
    /// nothing else would take the detail back up or let analysis go again.
    bool changed = false;
};

/// Notes what is moving the region, from what each frame knows about it.
class RegionMotionTracker
{
public:
    /// One per-frame step.
    RegionMotionStep update(const RegionMotionInputs& inputs);

    [[nodiscard]] RegionMotion motion() const
    {
        return m_motion;
    }

private:
    /// When the user last moved the region themselves. Held as an optional
    /// rather than a sentinel because the frame clock legitimately reads zero
    /// at startup.
    std::optional<double> m_draggedAt;
    RegionMotion m_motion = RegionMotion::Still;
};

}  // namespace sidescopes
