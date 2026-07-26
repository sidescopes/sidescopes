#pragma once

namespace sidescopes {

/// The frame period the loop aims at while something on screen is moving. New
/// scope images cannot arrive faster than the capture cadence, so redrawing at
/// the display's refresh rate spends whole frames on an identical image -
/// measured on a 120 Hz panel, 120 frames a second against 30 analysis passes,
/// three of every four redrawing nothing new.
inline constexpr double ContentRedrawSeconds = 1.0 / 30.0;

/// The frame period the loop aims at when the colour readout is the only thing
/// following the pointer. A swatch and a percentage carry no motion, so they
/// read the same at a fraction of the rate a marker easing across a trace
/// needs - and the pointer is outside the region, where no marker need be
/// drawn, for most of a working session.
///
/// It matches ReadoutSampleSeconds in cursor_sampler.h, which is what a test
/// pins: a frame drawn between two probes of the pointer redraws a swatch that
/// cannot have changed.
inline constexpr double ReadoutRedrawSeconds = 1.0 / 8.0;

/// How long the loop waits for events when nothing at all is happening.
inline constexpr double IdleWaitSeconds = 0.5;

/// How long since the last activity counts as nothing happening.
inline constexpr double IdleAfterSeconds = 0.5;

/// How long nothing must have asked for frames before the pipeline is
/// suspended. Long enough that stepping through the application switcher, a
/// hide the user immediately undoes, or a region cleared on the way to drawing
/// the next one, does not pay for a stream restart on the way back.
inline constexpr double CapturePauseSeconds = 0.75;

/// How the frame loop should block for events. The frame period is a floor
/// under all three - see FrameWaitDecision::redrawFloorSeconds.
enum class FrameWait
{
    /// Not at all: something is moving, so the frame period below is the whole
    /// of the wait and the events that land during it are drained at its end.
    None,
    /// In short slices, so an attached window's motion and focus stay fresh.
    WatchAttachedWindow,
    /// At the slow idle tick, which the first event to arrive ends.
    Idle,
};

/// The wait the loop should take.
struct FrameWaitDecision
{
    FrameWait kind = FrameWait::Idle;
    /// Seconds before the next frame may be drawn, whatever ends the wait
    /// early. A blocking wait returns on the first event that arrives, and
    /// events arrive in bursts - a pointer crossing the window delivers one
    /// every few milliseconds - so without this the loop redraws at the event
    /// rate precisely when it decided nothing was happening: measured at 65
    /// frames a second for a picture that never changed.
    double redrawFloorSeconds = 0.0;
};

/// What the loop knows when it decides how to wait.
struct FramePacingInputs
{
    double now = 0.0;
    /// When anything last happened - new scope output, or interaction.
    double lastActivity = 0.0;
    /// When the colour readout last moved, which asks for frames of its own at
    /// a slower cadence.
    double lastReadoutActivity = 0.0;
    /// When the previous frame's event pump returned.
    double lastFrameStart = 0.0;
    bool attached = false;
    bool pickerActive = false;
};

/// The wait to take before the next frame.
///
/// The floor is what is left of the frame period rather than a fixed slice on
/// top of it: presenting already blocks - on the drawable through Metal, on the
/// composition tick through DwmFlush - so a fixed wait would add to that block
/// and halve the rate again on every display at or below 60 Hz. Aiming at the
/// period caps a faster panel and leaves a slower one exactly as it was. A
/// blocking wait still returns the instant an event arrives, so interaction is
/// handled at the rate it comes in; only frames nobody asked for are dropped.
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
    /// No region is selected, so no pass is being run on the frames and the
    /// only thing left reading them is the colour under the pointer - which
    /// has an off-stream sample of its own, the one the second display and
    /// Windows already use.
    bool nothingSelected = false;
    /// The picker or a face probe is reading frames on its own, so the stream
    /// must not be pulled out from under it.
    bool needsFrames = false;
};

/// Whether nothing is asking the capture for frames.
[[nodiscard]] bool nothingNeedsFrames(const VisibilityInputs& inputs);

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
    /// Whether nothing has been asking for frames, and since when. The flag is
    /// separate from the timestamp because a clock that legitimately reads zero
    /// - which the frame clock does at startup - is indistinguishable from a
    /// sentinel.
    bool m_idle = false;
    double m_idleSince = 0.0;
};

}  // namespace sidescopes
