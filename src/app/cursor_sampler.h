#pragma once

#include <memory>
#include <mutex>
#include <optional>

#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/marker_smoother.h"
#include "platform/desktop.h"

namespace sidescopes {

class CaptureController;

/// How fast each trace's marker follows the pointer, in milliseconds: the
/// per-scope smoothing the drawing asks for. Passed in rather than read from a
/// view, because the sample is host-wide truth while the smoothing belongs to
/// whatever is being drawn with it.
struct CursorSmoothing
{
    float vectorscopeMs = 0.0f;
    float waveformMs = 0.0f;
};

/// What one cursor sample yielded: the color under the pointer smoothed at
/// each trace's own rhythm, empty until a sample lands, and whether the
/// pointer moved far enough to count as interaction.
struct CursorSample
{
    std::optional<FloatColor> vectorscopeColor;
    std::optional<FloatColor> waveformColor;
    /// A marker moved: the host stamps its activity clock so the frame loop
    /// redraws at the moving cadence.
    ///
    /// The colour, not the pointer. What these samples feed is a marker drawn at
    /// the cursor's COLOUR on the vectorscope and waveform, so it only moves when
    /// that colour changes - sliding across a flat area of a photograph moves the
    /// pointer hundreds of times and the marker not at all. Waking on the pointer
    /// instead measured a jump from ten frames a second to sixty-five for a
    /// picture that never changed.
    bool changed = false;
};

/// Reads the color under the pointer wherever it is and smooths it per trace.
/// On the captured display it reads the capture stream's own frame; on every
/// other display a throttled one-shot screen sample keeps the readout alive
/// while capture is paused. It reads the capture controller and worker it is
/// constructed with and drives the platform desktop seams directly; the
/// smoothed colors travel back as a CursorSample the host hands to its
/// drawing.
class CursorSampler
{
public:
    /// @p capture names the streamed display and reports capture liveness, and
    /// @p worker supplies the frame the on-display sample reads. Both must
    /// outlive the sampler.
    CursorSampler(const CaptureController& capture, const AnalysisWorker& worker);

    /// One per-frame step. @p frameSize is the captured frame the pointer is
    /// mapped into, @p smoothing how fast each trace follows it, @p now the
    /// frame clock the off-display sample is throttled against, and
    /// @p deltaSeconds the frame's own length, which the smoothing advances by.
    [[nodiscard]] CursorSample update(std::optional<AnalysisWorker::FrameSize> frameSize, CursorSmoothing smoothing,
                                      double now, float deltaSeconds);

    /// The throttled cross-display sample under its lock, passed to the
    /// picker's pin tool each poll.
    [[nodiscard]] std::optional<FloatColor> screenSampleColor() const;

private:
    // The freshest cross-display sample: the async sampler's callback may land
    // on any thread, and may still be in flight at shutdown, so the state it
    // writes is shared ownership.
    struct ScreenSample
    {
        std::mutex mutex;
        std::optional<FloatColor> color;
    };

    /// The sample under the pointer on the captured display, read out of the
    /// capture stream's own newest frame.
    [[nodiscard]] std::optional<FloatColor> sampleCapturedFrame(DesktopPoint cursor,
                                                                AnalysisWorker::FrameSize frameSize) const;

    /// The sample under the pointer anywhere else: a throttled one-shot screen
    /// read whose result lands asynchronously, so this returns the freshest
    /// one to have arrived.
    [[nodiscard]] std::optional<FloatColor> sampleOtherDisplay(DesktopPoint cursor, double now);

    const CaptureController& m_capture;
    const AnalysisWorker& m_worker;

    MarkerSmoother m_vectorscopeMarker;
    MarkerSmoother m_waveformMarker;

    std::shared_ptr<ScreenSample> m_screenSample = std::make_shared<ScreenSample>();
    double m_nextScreenSample = 0.0;
    /// The marker colours the previous frame drew, so a frame is only spent when
    /// one of them actually moves.
    std::optional<FloatColor> m_lastVectorscopeColor;
    std::optional<FloatColor> m_lastWaveformColor;
};

}  // namespace sidescopes
