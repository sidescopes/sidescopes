#include "app/cursor_sampler.h"

#include <cmath>

#include "app/capture_controller.h"

namespace sidescopes {

namespace {

// Whether a marker's colour moved enough to be worth a frame. An eighth of a
// 0-255 code: below that the marker lands on the same pixel of the scope, and
// the smoothing would otherwise creep towards its target for ever at sixty
// frames a second, never quite arriving.
bool markerMoved(const std::optional<FloatColor>& before, const std::optional<FloatColor>& after)
{
    if (before.has_value() != after.has_value()) {
        return true;
    }
    if (!after) {
        return false;
    }
    constexpr float Threshold = 1.0f / (255.0f * 8.0f);

    return std::abs(before->r - after->r) > Threshold || std::abs(before->g - after->g) > Threshold ||
           std::abs(before->b - after->b) > Threshold;
}

}  // namespace

CursorSampler::CursorSampler(const CaptureController& capture, const AnalysisWorker& worker)
    : m_capture(capture),
      m_worker(worker)
{
}

std::optional<FloatColor> CursorSampler::screenSampleColor() const
{
    std::lock_guard lock(m_screenSample->mutex);

    return m_screenSample->color;
}

std::optional<CursorSampler::DisplayPixel> CursorSampler::displayPixelOf(DesktopPoint cursor,
                                                                         AnalysisWorker::FrameSize frameSize) const
{
    const auto geometry = geometryOfDisplay(m_capture.capturedDisplay());
    if (!geometry) {
        return std::nullopt;
    }
    // Display pixels, so a capture narrowed to the analysis region still reads
    // the point the cursor is actually over; the frame maps it back itself.
    return DisplayPixel{
        static_cast<int>((cursor.x - geometry->originX) * frameSize.displayWidth / geometry->widthPoints),
        static_cast<int>((cursor.y - geometry->originY) * frameSize.displayHeight / geometry->heightPoints)};
}

std::optional<FloatColor> CursorSampler::sampleCapturedFrame(DisplayPixel pixel) const
{
    return m_worker.sampleDisplayColor(pixel.x, pixel.y);
}

std::optional<FloatColor> CursorSampler::sampleOtherDisplay(DesktopPoint cursor, double now)
{
    if (now > m_nextScreenSample) {
        m_nextScreenSample = now + 0.05;
        auto screenSample = m_screenSample;
        sampleScreenColorAsync(cursor, [screenSample](std::optional<FloatColor> color) {
            if (!color) {
                return;
            }
            std::lock_guard lock(screenSample->mutex);
            screenSample->color = color;
        });
    }

    return screenSampleColor();
}

CursorSample CursorSampler::update(std::optional<AnalysisWorker::FrameSize> frameSize, const RegionOfInterest& region,
                                   CursorSmoothing smoothing, double now, float deltaSeconds)
{
    // Cursor color, smoothed per scope with its own rhythm. On the captured
    // display it reads the capture stream's frame; on every other display a
    // throttled one-shot sample keeps the readout alive even while capture is
    // paused.
    CursorSample sample;
    if (m_capture.capturedDisplay() == 0) {
        return sample;
    }
    const auto cursor = globalCursorPosition();
    if (!cursor) {
        return sample;
    }
    const bool onCapturedDisplay = displayAtPoint(*cursor).value_or(0) == m_capture.capturedDisplay();
    const std::optional<DisplayPixel> pixel =
        onCapturedDisplay && frameSize ? displayPixelOf(*cursor, *frameSize) : std::nullopt;
    std::optional<FloatColor> sampled;
    if (pixel && !m_capture.dead()) {
        sampled = sampleCapturedFrame(*pixel);
    }
    if (!sampled) {
        // Either another display, or a point the capture no longer carries:
        // narrowed to the analysis region, the stream holds nothing outside it.
        sampled = sampleOtherDisplay(*cursor, now);
    }
    if (!sampled) {
        return sample;
    }
    m_readout.setTimeConstant(smoothing.vectorscopeMs);
    sample.readoutColor = m_readout.update(*sampled, deltaSeconds);
    sample.readoutChanged = markerMoved(m_lastReadoutColor, sample.readoutColor);
    m_lastReadoutColor = sample.readoutColor;

    const bool inRegion =
        pixel && frameSize &&
        region.toPixels(frameSize->displayWidth, frameSize->displayHeight).contains(pixel->x, pixel->y);
    advanceMarkers(markerTarget(sampled, inRegion, now), smoothing, deltaSeconds, sample);

    return sample;
}

std::optional<FloatColor> CursorSampler::markerTarget(const std::optional<FloatColor>& sampled, bool inRegion,
                                                      double now)
{
    if (!inRegion) {
        // Nothing to travel towards, and the next arrival takes its colour
        // where the pointer comes back rather than where it left.
        m_markerTarget.reset();
        m_nextMarkerSample = 0.0;

        return std::nullopt;
    }
    if (now >= m_nextMarkerSample || !m_markerTarget) {
        m_markerTarget = sampled;
        m_nextMarkerSample = now + MarkerSampleSeconds;
    }

    return m_markerTarget;
}

void CursorSampler::advanceMarkers(const std::optional<FloatColor>& target, CursorSmoothing smoothing,
                                   float deltaSeconds, CursorSample& sample)
{
    // A marker only ever stands for a colour inside the region the scopes are
    // reading. Outside it there is no marker rather than a stale one, and the
    // smoothing forgets where it was so the next one appears where the pointer
    // is instead of sweeping there.
    if (target) {
        m_vectorscopeMarker.setTimeConstant(smoothing.vectorscopeMs);
        m_waveformMarker.setTimeConstant(smoothing.waveformMs);
        sample.vectorscopeColor = m_vectorscopeMarker.update(*target, deltaSeconds);
        sample.waveformColor = m_waveformMarker.update(*target, deltaSeconds);
    } else {
        m_vectorscopeMarker.forget();
        m_waveformMarker.forget();
    }
    // The smoothed value, not the raw one: a marker easing towards a colour it
    // has not reached yet still has to be redrawn, and one that has arrived
    // does not.
    sample.changed = markerMoved(m_lastVectorscopeColor, sample.vectorscopeColor) ||
                     markerMoved(m_lastWaveformColor, sample.waveformColor);
    m_lastVectorscopeColor = sample.vectorscopeColor;
    m_lastWaveformColor = sample.waveformColor;
}

}  // namespace sidescopes
