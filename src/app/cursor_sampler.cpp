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

std::optional<FloatColor> CursorSampler::sampleCapturedFrame(DesktopPoint cursor,
                                                             AnalysisWorker::FrameSize frameSize) const
{
    const auto geometry = geometryOfDisplay(m_capture.capturedDisplay());
    if (!geometry) {
        return std::nullopt;
    }
    // Display pixels, so a capture narrowed to the analysis region still reads
    // the point the cursor is actually over; the frame maps it back itself.
    const int pixelX =
        static_cast<int>((cursor.x - geometry->originX) * frameSize.displayWidth / geometry->widthPoints);
    const int pixelY =
        static_cast<int>((cursor.y - geometry->originY) * frameSize.displayHeight / geometry->heightPoints);

    return m_worker.sampleDisplayColor(pixelX, pixelY);
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

CursorSample CursorSampler::update(std::optional<AnalysisWorker::FrameSize> frameSize, CursorSmoothing smoothing,
                                   double now, float deltaSeconds)
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
    std::optional<FloatColor> sampled;
    if (onCapturedDisplay && !m_capture.dead() && frameSize) {
        sampled = sampleCapturedFrame(*cursor, *frameSize);
    }
    if (!sampled) {
        // Either another display, or a point the capture no longer carries:
        // narrowed to the analysis region, the stream holds nothing outside it.
        sampled = sampleOtherDisplay(*cursor, now);
    }
    if (sampled) {
        m_vectorscopeMarker.setTimeConstant(smoothing.vectorscopeMs);
        m_waveformMarker.setTimeConstant(smoothing.waveformMs);
        sample.vectorscopeColor = m_vectorscopeMarker.update(*sampled, deltaSeconds);
        sample.waveformColor = m_waveformMarker.update(*sampled, deltaSeconds);
        // The smoothed value, not the raw one: a marker easing towards a colour
        // it has not reached yet still has to be redrawn, and one that has
        // arrived does not.
        sample.changed = markerMoved(m_lastVectorscopeColor, sample.vectorscopeColor) ||
                         markerMoved(m_lastWaveformColor, sample.waveformColor);
        m_lastVectorscopeColor = sample.vectorscopeColor;
        m_lastWaveformColor = sample.waveformColor;
    }

    return sample;
}

}  // namespace sidescopes
