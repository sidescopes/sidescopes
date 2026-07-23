#include "app/cursor_sampler.h"

#include <cmath>

#include "app/capture_controller.h"

namespace sidescopes {

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
    const int pixelX = static_cast<int>((cursor.x - geometry->originX) * frameSize.width / geometry->widthPoints);
    const int pixelY = static_cast<int>((cursor.y - geometry->originY) * frameSize.height / geometry->heightPoints);

    return m_worker.sampleFrameColor(pixelX, pixelY);
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
    if (std::abs(cursor->x - m_lastCursor.x) + std::abs(cursor->y - m_lastCursor.y) > 0.5) {
        m_lastCursor = *cursor;
        sample.moved = true;
    }
    const bool onCapturedDisplay = displayAtPoint(*cursor).value_or(0) == m_capture.capturedDisplay();
    std::optional<FloatColor> sampled;
    if (onCapturedDisplay && !m_capture.dead() && frameSize) {
        sampled = sampleCapturedFrame(*cursor, *frameSize);
    } else {
        sampled = sampleOtherDisplay(*cursor, now);
    }
    if (sampled) {
        m_vectorscopeMarker.setTimeConstant(smoothing.vectorscopeMs);
        m_waveformMarker.setTimeConstant(smoothing.waveformMs);
        sample.vectorscopeColor = m_vectorscopeMarker.update(*sampled, deltaSeconds);
        sample.waveformColor = m_waveformMarker.update(*sampled, deltaSeconds);
    }

    return sample;
}

}  // namespace sidescopes
