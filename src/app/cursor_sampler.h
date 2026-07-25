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

/// How often a marker takes a new colour to travel towards. The pointer crosses
/// a photograph faster than a marker means anything at: sampled every frame, a
/// marker chases thirty colours a second and reads as jumping about rather than
/// as a reading. Twelve a second is a third of that, and still short enough
/// against the traces' own smoothing - 75 ms on the vectorscope, 100 on the
/// waveform - that a new colour arrives while the marker is still travelling,
/// so it keeps moving continuously instead of stepping and settling.
inline constexpr double MarkerSampleSeconds = 1.0 / 12.0;

/// How often the readout takes a new colour. It has the same complaint as the
/// marker and less reason to move at all: a swatch and three percentages
/// following the pointer thirty times a second flicker through every pixel on
/// the way to the one being looked at, and each of those readings costs a frame.
/// Eight a second is under a sixth of a second to land on a colour deliberately
/// pointed at, and a quarter of the probes.
inline constexpr double ReadoutSampleSeconds = 1.0 / 8.0;

/// Whether a marker stands only for a colour taken inside the region the scopes
/// are reading.
///
/// It does not: the region and the pointer are separate inputs. The region
/// decides what the traces are BUILT from, and a marker is a live probe of what
/// is under the pointer, which is worth having with no region drawn at all -
/// hovering over a face to see whether it sits on the skin-tone line is a
/// reading in itself. The readout and the colour picker's swatch follow the same
/// rule, so all three agree about what the pointer means.
inline constexpr bool MarkersFollowRegion = false;

/// How fast each trace's marker follows the pointer, in milliseconds: the
/// per-scope smoothing the drawing asks for. Passed in rather than read from a
/// view, because the sample is host-wide truth while the smoothing belongs to
/// whatever is being drawn with it.
struct CursorSmoothing
{
    float vectorscopeMs = 0.0f;
    float waveformMs = 0.0f;
};

/// What one cursor sample yielded: the marker colors and the readout color,
/// which all follow the pointer wherever it goes, and whether either moved.
struct CursorSample
{
    /// The color a trace marks, smoothed at that trace's own rhythm. Empty
    /// until a sample lands, and empty wherever the pointer carries no marker -
    /// absent rather than frozen, since a marker left where the pointer last
    /// was is stale and plausible at once.
    std::optional<FloatColor> vectorscopeColor;
    std::optional<FloatColor> waveformColor;
    /// The color under the pointer wherever it is, for the readout: the swatch,
    /// the channel percentages and the color picker's own pane, which measure a
    /// point on screen rather than a place in a distribution.
    std::optional<FloatColor> readoutColor;
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
    /// The readout's colour moved. Its own signal because a swatch and a
    /// number carry no motion: the host follows them at a slower cadence than
    /// a marker easing across a trace.
    bool readoutChanged = false;
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
    /// mapped into, @p region the region the scopes are reading - empty for
    /// none - which decides whether the pointer carries a marker when markers
    /// are scoped to it, @p smoothing how fast each trace follows it, @p now
    /// the frame clock the off-display sample is throttled against, and
    /// @p deltaSeconds the frame's own length, which the smoothing advances by.
    [[nodiscard]] CursorSample update(std::optional<AnalysisWorker::FrameSize> frameSize,
                                      const std::optional<RegionOfInterest>& region, CursorSmoothing smoothing,
                                      double now, float deltaSeconds);

    /// The throttled cross-display sample under its lock, passed to the
    /// picker's pin tool each poll.
    [[nodiscard]] std::optional<FloatColor> screenSampleColor() const;

    /// Switches the marker scope - see MarkersFollowRegion, which is what this
    /// starts as. The application itself never calls it; it exists so the scope
    /// is one predicate rather than a shape, and so both readings stay
    /// exercised.
    void setMarkersFollowRegion(bool followRegion)
    {
        m_markersFollowRegion = followRegion;
    }

private:
    // The freshest cross-display sample: the async sampler's callback may land
    // on any thread, and may still be in flight at shutdown, so the state it
    // writes is shared ownership.
    struct ScreenSample
    {
        std::mutex mutex;
        std::optional<FloatColor> color;
    };

    /// A point on the captured display, in that display's own pixels - the
    /// coordinates the region and every frame mapping are stated in.
    struct DisplayPixel
    {
        int x = 0;
        int y = 0;
    };

    /// Where @p cursor sits in the captured display's own pixels, or nothing
    /// when that display's geometry is unknown.
    [[nodiscard]] std::optional<DisplayPixel> displayPixelOf(DesktopPoint cursor,
                                                             AnalysisWorker::FrameSize frameSize) const;

    /// The sample under the pointer on the captured display, read out of the
    /// capture stream's own newest frame.
    [[nodiscard]] std::optional<FloatColor> sampleCapturedFrame(DisplayPixel pixel) const;

    /// The colour under the pointer, from the capture stream where it reaches
    /// and from a one-shot screen read where it does not.
    [[nodiscard]] std::optional<FloatColor> probeColor(DesktopPoint cursor, const std::optional<DisplayPixel>& pixel,
                                                       double now);

    /// Advances the readout one frame, taking @p probed as its new target when
    /// one is due, and reports it through @p sample.
    void updateReadout(const std::optional<FloatColor>& probed, CursorSmoothing smoothing, double now,
                       float deltaSeconds, CursorSample& sample);

    /// The colour the markers are travelling towards: @p sampled taken afresh
    /// when the sampling interval is up, the one they were already following
    /// until then, and nothing at all while @p markersLive is false.
    [[nodiscard]] std::optional<FloatColor> markerTarget(const std::optional<FloatColor>& sampled, bool markersLive,
                                                         double now);

    /// Advances both trace markers one frame towards @p target, or takes them
    /// off the traces when it is empty, and reports through @p sample whether
    /// either moved.
    void advanceMarkers(const std::optional<FloatColor>& target, CursorSmoothing smoothing, float deltaSeconds,
                        CursorSample& sample);

    /// The sample under the pointer anywhere else: a throttled one-shot screen
    /// read whose result lands asynchronously, so this returns the freshest
    /// one to have arrived.
    [[nodiscard]] std::optional<FloatColor> sampleOtherDisplay(DesktopPoint cursor, double now);

    const CaptureController& m_capture;
    const AnalysisWorker& m_worker;

    MarkerSmoother m_vectorscopeMarker;
    MarkerSmoother m_waveformMarker;
    /// The readout's own smoothing, at the vectorscope's rhythm: the swatch and
    /// the picker's hex have always eased at that rate, and they keep it now
    /// that the marker they used to borrow it from can be absent.
    MarkerSmoother m_readout;

    std::shared_ptr<ScreenSample> m_screenSample = std::make_shared<ScreenSample>();
    double m_nextScreenSample = 0.0;
    /// The colours the markers and the readout are travelling towards, and when
    /// each is due a new one: what makes them readings taken a dozen times a
    /// second rather than points chasing every frame's pixel.
    std::optional<FloatColor> m_markerTarget;
    double m_nextMarkerSample = 0.0;
    std::optional<FloatColor> m_readoutTarget;
    double m_nextReadoutSample = 0.0;
    bool m_markersFollowRegion = MarkersFollowRegion;
    /// The colours the previous frame drew, so a frame is only spent when one of
    /// them actually moves.
    std::optional<FloatColor> m_lastVectorscopeColor;
    std::optional<FloatColor> m_lastWaveformColor;
    std::optional<FloatColor> m_lastReadoutColor;
};

}  // namespace sidescopes
