#pragma once

namespace sidescopes {

/// The frame period the loop aims at while something on screen is moving. New
/// scope images cannot arrive faster than the capture cadence, so redrawing at
/// the display's refresh rate spends whole frames on an identical image -
/// measured on a 120 Hz panel, 120 frames a second against 30 analysis passes,
/// three of every four redrawing nothing new.
inline constexpr double ContentRedrawSeconds = 1.0 / 30.0;

/// How long the loop waits for events when nothing at all is happening.
inline constexpr double IdleWaitSeconds = 0.5;

/// How long since the last activity counts as nothing happening.
inline constexpr double IdleAfterSeconds = 0.5;

/// How long the window must stay out of sight before the pipeline is
/// suspended. Long enough that stepping through the application switcher, or a
/// hide the user immediately undoes, does not pay for a stream restart on the
/// way back.
inline constexpr double OutOfSightPauseSeconds = 0.75;

/// What the frame loop should do to wait for the next frame.
enum class FrameWait
{
    /// Nothing is happening, and an attached window needs watching in short
    /// slices so its motion and focus stay fresh.
    WatchAttachedWindow,
    /// Nothing is happening: wait for events at the slow idle tick.
    Idle,
    /// Something is moving: wait out whatever is left of the frame period.
    UntilFramePeriod,
};

/// The wait the loop should take, and for how long.
struct FrameWaitDecision
{
    FrameWait kind = FrameWait::Idle;
    /// Seconds to wait; zero means poll without blocking.
    double seconds = 0.0;
};

/// What the loop knows when it decides how to wait.
struct FramePacingInputs
{
    double now = 0.0;
    /// When anything last happened - new scope output, or interaction.
    double lastActivity = 0.0;
    /// When the previous frame's event pump returned.
    double lastFrameStart = 0.0;
    bool attached = false;
    bool pickerActive = false;
};

/// The wait to take before the next frame.
///
/// The moving case waits out what is left of the frame period rather than
/// adding a fixed slice on top of it: presenting already blocks - on the
/// drawable through Metal, on the composition tick through DwmFlush - so a
/// fixed wait would add to that block and halve the rate again on every
/// display at or below 60 Hz. Aiming at the period caps a faster panel and
/// leaves a slower one exactly as it was. Any wait returns the instant an
/// event arrives, so a drag or a hover still runs at the rate its events come
/// in; only frames nobody asked for are dropped.
[[nodiscard]] FrameWaitDecision frameWaitFor(const FramePacingInputs& inputs);

/// What the loop knows about whether anything is worth computing.
struct VisibilityInputs
{
    /// The session has stopped showing anything: the display asleep, the
    /// screen locked, another user switched in.
    bool sessionAsleep = false;
    /// The application is hidden the platform's way.
    bool applicationHidden = false;
    bool iconified = false;
    bool windowVisible = true;
    /// The framebuffer has no area, so there is nothing to draw into either.
    bool framebufferEmpty = false;
    /// The picker or a face probe is reading frames on its own, so the stream
    /// must not be pulled out from under it.
    bool needsFrames = false;
};

/// Whether nothing on screen is showing the scopes.
[[nodiscard]] bool outOfSight(const VisibilityInputs& inputs);

/// What the frame loop should do with the capture pipeline.
enum class PipelineAction
{
    /// Leave it as it is.
    Keep,
    /// Stop capturing: nothing is on screen to show it.
    Suspend,
    /// Start capturing again.
    Resume,
};

/// Decides whether to suspend or resume the pipeline, and carries the clock
/// the hysteresis measures against.
///
/// Suspending stops the whole pipeline behind the stream - the backend's own
/// work, the per-frame copy, change detection and the analysis pass - and with
/// no output arriving the frame loop falls to its idle tick as well.
class VisibilityGate
{
public:
    /// @return What to do now. @p suspended is the pipeline's current state, so
    /// the gate never asks for a transition that has already happened.
    [[nodiscard]] PipelineAction update(const VisibilityInputs& inputs, bool suspended, double now);

private:
    /// Whether the window is out of sight, and since when. The flag is separate
    /// from the timestamp because a clock that legitimately reads zero - which
    /// the frame clock does at startup - is indistinguishable from a sentinel.
    bool m_outOfSight = false;
    double m_outOfSightSince = 0.0;
};

}  // namespace sidescopes
