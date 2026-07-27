#pragma once

#include <atomic>
#include <string>
#include <string_view>

namespace sidescopes {

/// The frame period the loop aims at while something on screen is moving. New
/// scope images cannot arrive faster than the capture cadence, so redrawing at
/// the display's refresh rate spends whole frames on an identical image -
/// measured on a 120 Hz panel, 120 frames a second against 30 analysis passes,
/// three of every four redrawing nothing new.
///
/// Twenty rather than thirty, and the cursor marker is what decides it: its
/// glide between samples animates on drawn frames, so the cap is a smoothness
/// setting as much as a cost one. Driven against the real MarkerSmoother at
/// both shipped time constants and three pointer speeds, twenty is as even as
/// thirty on every one - a tenth of frames off the average step, fastest 1.38
/// of it against 1.37 - and it settles sooner, within a code in 100-150 ms
/// against 133-167. Twenty-five and fifteen are both worse than either,
/// because what matters is how the period divides the marker's own 83 ms
/// sampling interval rather than how large it is: at fifteen the marker gets
/// one frame a sample and steps, and at twenty-five the two rates beat.
inline constexpr double ContentRedrawSeconds = 1.0 / 20.0;

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

/// The longest the loop blocks while the user's hand is on the region.
///
/// Not a frame period and not a floor: the wait ends on the pointer event that
/// arrives, and this only paces a hand that has paused mid-gesture. It is the
/// content period because a paused hand wants exactly what a moving screen
/// wants.
inline constexpr double InteractionWaitSeconds = ContentRedrawSeconds;

/// How long the loop waits for events when nothing at all is happening.
inline constexpr double IdleWaitSeconds = 0.5;

/// How long since the last activity counts as nothing happening.
inline constexpr double IdleAfterSeconds = 0.5;

/// How long after the last window event the loop keeps drawing. Hover
/// highlights, tooltip delays and text carets advance only on a drawn frame,
/// and the interface toolkit's own hover delays run to a third of a second
/// from the moment the pointer settles - so the frames that finish an
/// interaction have to outlast the interaction itself.
inline constexpr double InputSettleSeconds = 1.0;

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
    /// Until the next event, and no longer than InteractionWaitSeconds: the
    /// user is drawing or dragging a region, and the loop follows their hand.
    FollowInteraction,
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
    /// When the pointer last moved. It asks for frames at the readout's cadence
    /// because what follows it is a reading - see RedrawInputs::lastPointerMove
    /// for why nothing else notices.
    double lastPointerMove = 0.0;
    /// When the previous frame's event pump returned.
    double lastFrameStart = 0.0;
    bool attached = false;
    bool pickerActive = false;
    /// The user's hand is on the region: a rubber band being drawn, or the
    /// border being dragged. See frameWaitFor for why it outranks everything
    /// else here.
    bool regionInteracting = false;
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
///
/// DIRECT MANIPULATION IS NEVER PACED. A region being drawn or dragged is
/// repositioned from this loop and from nowhere else - the overlay records the
/// pointer, the loop applies it and moves the border - so a floor here is a
/// floor on how closely the border can follow the hand, and it is measurable:
/// at a frame period of 50 ms a border flicked at 1600 points a second trailed
/// the pointer by 83 ms on average and 158 at worst. The interaction is a
/// second or two long, so what it costs is not worth a moment of its
/// stickiness.
[[nodiscard]] FrameWaitDecision frameWaitFor(const FramePacingInputs& inputs);

/// What the loop knows about whether the picture can still be changing.
struct RedrawInputs
{
    double now = 0.0;
    /// When anything last happened - new scope output, or interaction.
    double lastActivity = 0.0;
    /// When the colour readout last moved.
    double lastReadoutActivity = 0.0;
    /// When the pointer last moved, wherever on the desktop it is.
    ///
    /// Its own clock because nothing else here can stand for it: a marker and
    /// the colour readout are readings of the point under the pointer, and the
    /// pointer crosses a still photograph in another application's window
    /// without changing one pixel of the screen or delivering this window one
    /// event. Every other reason listed here would say the picture is finished,
    /// and the readings would sit on the colour they were last drawn at.
    double lastPointerMove = 0.0;
    /// When a window event from the user last arrived.
    double lastInputEvent = 0.0;
    /// When the last frame was drawn.
    double lastDrawn = 0.0;
    /// When something standing on screen leaves it by itself - a status
    /// message, the attach notice, an intensity readout - or zero while
    /// nothing timed has ever been shown.
    double redrawDue = 0.0;
    /// The worker has published a pass no frame has shown yet.
    bool outputPending = false;
    /// A text cursor is blinking, which changes the picture with no input.
    bool textInputActive = false;
    /// A picker overlay is up, and the frame it draws is what keeps its
    /// colour readout current.
    bool overlayActive = false;
    /// The window's size differs from the one last drawn into.
    bool framebufferChanged = false;
    /// The capture status differs from the one last drawn.
    bool statusChanged = false;
    /// The user's hand is on the region. The wait no longer paces the loop
    /// while this holds, so the frame period is enforced here instead.
    bool regionInteracting = false;
};

/// Whether a frame is worth drawing at all.
///
/// While this is false nothing is presented, which is the whole point rather
/// than a saving on the side: the graphics driver holds a per-process render
/// arena - measured at 85-99 MB, most of it wired - for as long as frames keep
/// arriving, and releases the bulk of it about a second after the last one.
/// Slowing down never reached that. Two frames a second held the entire arena
/// resident, so the loop has to stop presenting outright.
[[nodiscard]] bool frameWorthDrawing(const RedrawInputs& inputs);

/// The facts a redraw decision rests on that are not clocks, gathered where
/// each is true rather than held: the capture status and the framebuffer size
/// are compared against what the last drawn frame was drawn from, and the rest
/// are read off the interface as the frame is decided.
struct RedrawSignals
{
    /// When a window event from the user last arrived.
    double lastInputEvent = 0.0;
    /// When something standing on screen leaves it by itself.
    double redrawDue = 0.0;
    bool textInputActive = false;
    bool overlayActive = false;
    /// The user's hand is on the region itself.
    bool regionInteracting = false;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    /// The line the capture would put in the status bar now.
    std::string_view captureStatus;
};

/// When each thing that can change the picture last happened, and what the
/// frame on screen was drawn from.
///
/// Both of the loop's decisions - how long to block, and whether to draw at
/// all - are functions of these and nothing else, so the clocks and the
/// policies that read them belong together. They used to be nine fields on the
/// shell, stamped from twenty call sites and read from three, which is a
/// decision no test can reach.
///
/// Every clock is supplied by the caller rather than taken here, so a test
/// drives the whole thing on a clock of its own; that is also the convention
/// the rest of the shell's timed units follow.
class FrameClocks
{
public:
    /// Anything happened that the picture follows: a published pass, a
    /// gesture, a menu choice, a border moved.
    void noteActivity(double now);

    /// The colour readout moved, which asks for frames at a slower cadence of
    /// its own.
    void noteReadoutActivity(double now);

    /// The pointer is somewhere it was not.
    void notePointerMove(double now);

    /// The event pump returned, which is what a frame period is counted from.
    void notePumpReturned(double now);

    /// A frame is being built. Stamped where the frame begins rather than
    /// where it ends, because it is what the frame period is counted from: a
    /// period measured from the end adds the frame's own length to every one
    /// of them, and the body plus the present were enough to take a
    /// twenty-frame cadence down to sixteen.
    void noteFrameBegun(double now);

    /// A frame was presented, built for @p framebufferWidth by
    /// @p framebufferHeight with @p captureStatus in its status line - so the
    /// next pass can tell whether the picture it would draw is the one already
    /// on screen.
    void noteFrameShown(int framebufferWidth, int framebufferHeight, std::string captureStatus);

    /// The worker published a pass no frame has shown yet. Called from the
    /// worker's own thread, so it touches nothing else.
    void noteOutputPublished();

    [[nodiscard]] FramePacingInputs pacingInputs(double now, bool attached, bool pickerActive,
                                                 bool regionInteracting) const;

    [[nodiscard]] RedrawInputs redrawInputs(const RedrawSignals& signals, double now) const;

    /// How long to block while following a hand on the region: no later than
    /// the next frame is due, and never longer than one interaction slice.
    ///
    /// Nothing else paces the drawing once the loop is following events, so a
    /// wait that ended only on an event drew every frame late by however long
    /// the gap to the next one was - a hand reporting every 20 ms took a
    /// twenty-frame cadence down to sixteen, and the scopes with it. A
    /// deadline already past belongs to the frame this pass is about to draw,
    /// so there is nothing left to wake for and the ceiling is the honest
    /// wait.
    [[nodiscard]] double interactionWait(double now) const;

private:
    double m_lastActivity = 0.0;
    double m_lastReadoutActivity = 0.0;
    double m_lastPointerMove = 0.0;
    double m_lastFrameStart = 0.0;
    double m_lastDrawnFrame = 0.0;
    int m_drawnFramebufferWidth = 0;
    int m_drawnFramebufferHeight = 0;
    std::string m_drawnCaptureStatus;
    std::atomic<bool> m_outputPending{false};
};

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
