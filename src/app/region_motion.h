#pragma once

#include <optional>

namespace sidescopes {

/// How long the region must sit still after the user lets go before the scopes
/// go back to full detail. Longer than the gap between two steps of a drag, so
/// one pass is not computed at full detail in the middle of one, and short
/// enough that letting go is followed by the sharp trace.
inline constexpr double RegionSettleSeconds = 0.25;

/// How fast the user's hand has to be moving the region before nobody could be
/// reading the scopes, as per cent of the display a second.
///
/// Two gestures, and the owner separates them: a region SCANNED at walking pace
/// hunting for blown highlights is read while it moves, and a region THROWN
/// from one face to another is not - "no one is going to care about the scopes
/// during this short period". Measured on the harness's own two: its slow scan
/// travels about 16 per cent of the display a second, its flick about 100. The
/// pair sits between them, far enough from the scan that a brisk one never
/// crosses it.
inline constexpr double ThrownSpeedPercent = 50.0;

/// And how far it must slow down to be read again. Lower than the speed that
/// starts a throw, so a hand that wavers around one number does not flick the
/// scopes on and off through the middle of a gesture.
inline constexpr double ScannedSpeedPercent = 25.0;

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
    /// How far the region moved to differ, as per cent of the display: the
    /// largest of its four edges' displacements, so a rectangle being thrown
    /// and one being drawn from a corner both read as the movement they are.
    /// Meaningless unless regionChanged.
    double travelPercent = 0.0;
};

/// What one step decided.
struct RegionMotionStep
{
    RegionMotion motion = RegionMotion::Still;
    /// Whether the hand is moving it too fast for anyone to be reading the
    /// scopes. Only ever true of a Dragged region - a window carrying one is
    /// already not being read at any speed.
    bool thrown = false;
    /// Whether either of the two above just changed. The change itself has to
    /// be carried out: a region that stopped moving stops dirtying the
    /// settings, so nothing else would take the detail back up or let analysis
    /// go again.
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
    /// Reads the hand's speed off one region change and answers whether the
    /// region is being thrown, with the hysteresis that keeps one gesture in
    /// one regime.
    [[nodiscard]] bool throwing(const RegionMotionInputs& inputs);

    /// When the user last moved the region themselves. Held as an optional
    /// rather than a sentinel because the frame clock legitimately reads zero
    /// at startup.
    std::optional<double> m_draggedAt;
    RegionMotion m_motion = RegionMotion::Still;
    bool m_thrown = false;
};

}  // namespace sidescopes
