#include "core/analysis_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/marker_smoother.h"
#include "core/region_hash.h"

namespace sidescopes {

IntRect RegionOfInterest::toPixels(int frameWidth, int frameHeight) const
{
    // Edges round INWARD: truncation used to include the pixel row just
    // outside the region at some fractional positions, and the region
    // border's own bright ring lives exactly there - it flickered into
    // the waveform as a phantom line near the top. A boundary pixel
    // belongs to the sample only when it is entirely inside.
    const auto floorEdge = [](double percent, int extent) {
        return static_cast<int>(std::floor(percent * extent / 100.0));
    };
    const auto ceilEdge = [](double percent, int extent) {
        return static_cast<int>(std::ceil(percent * extent / 100.0));
    };
    const int left = ceilEdge(leftPercent, frameWidth);
    const int top = ceilEdge(topPercent, frameHeight);
    const int right = std::max(left, floorEdge(rightPercent, frameWidth));
    const int bottom = std::max(top, floorEdge(bottomPercent, frameHeight));
    return IntRect{left, top, right - left, bottom - top};
}

AnalysisWorker::AnalysisWorker(FrameMailbox& mailbox)
    : m_mailbox(mailbox)
{
}

AnalysisWorker::~AnalysisWorker()
{
    stop();
}

void AnalysisWorker::start()
{
    if (m_thread.joinable()) {
        return;
    }
    m_stopRequested.store(false);
    m_thread = std::thread(&AnalysisWorker::run, this);
}

void AnalysisWorker::stop()
{
    if (!m_thread.joinable()) {
        return;
    }
    m_stopRequested.store(true);
    m_thread.join();
}

void AnalysisWorker::updateSettings(const AnalysisSettings& settings)
{
    {
        std::lock_guard lock(m_settingsMutex);
        m_settings = settings;
        ++m_settingsVersion;
    }
    // Without the nudge a settings change waits out the frame take's
    // timeout on a static screen - up to 100 ms of stale scope images
    // after a settings or visibility change.
    m_mailbox.nudge();
}

bool AnalysisWorker::fetchOutput(uint64_t& lastSeenVersion, Output& output) const
{
    std::lock_guard lock(m_outputMutex);
    if (m_output.version == lastSeenVersion) {
        return false;
    }
    output = m_output;
    lastSeenVersion = m_output.version;
    return true;
}

std::optional<FloatColor> AnalysisWorker::sampleFrameColor(int px, int py, int radius) const
{
    std::lock_guard lock(m_frameMutex);
    if (!m_hasFrame) {
        return std::nullopt;
    }
    const FrameView view = m_latestFrame.view();
    if (px < 0 || px >= view.width || py < 0 || py >= view.height) {
        return std::nullopt;
    }
    return averageNeighborhood(view, px, py, radius);
}

bool AnalysisWorker::withLatestFrame(const std::function<void(const FrameView&)>& reader) const
{
    std::lock_guard lock(m_frameMutex);
    if (!m_hasFrame) {
        return false;
    }
    reader(m_latestFrame.view());
    return true;
}

std::optional<AnalysisWorker::FrameSize> AnalysisWorker::latestFrameSize() const
{
    std::lock_guard lock(m_frameMutex);
    if (!m_hasFrame) {
        return std::nullopt;
    }
    return FrameSize{m_latestFrame.width, m_latestFrame.height};
}

void AnalysisWorker::run()
{
    Vectorscope vectorscope;
    Waveform waveform;
    Waveform waveformParade;
    Histogram histogram;
    AnalysisSettings settings;
    uint64_t seenSettingsVersion = 0;
    uint64_t lastContentHash = 0;
    uint64_t framesProcessed = 0;

    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        bool newFrame = false;
        if (auto frame = m_mailbox.takeLatest(std::chrono::milliseconds(100))) {
            std::lock_guard lock(m_frameMutex);
            if (m_hasFrame) {
                m_mailbox.returnStorage(std::move(m_latestFrame));
            }
            m_latestFrame = std::move(*frame);
            m_hasFrame = true;
            newFrame = true;
        }

        bool settingsChanged = false;
        {
            std::lock_guard lock(m_settingsMutex);
            if (m_settingsVersion != seenSettingsVersion) {
                settings = m_settings;
                seenSettingsVersion = m_settingsVersion;
                settingsChanged = true;
            }
        }

        {
            std::lock_guard lock(m_frameMutex);
            if (!m_hasFrame || (!newFrame && !settingsChanged)) {
                continue;
            }
        }

        // Reading the frame without the lock is safe: this thread is the
        // only writer, and readers on other threads take the mutex only for
        // the brief sampling reads that tolerate the previous frame.
        const FrameView view = m_latestFrame.view();
        const IntRect region = settings.region.toPixels(view.width, view.height).clampedTo(view.width, view.height);

        // The hash is computed on every pass — including settings-only ones —
        // so it always corresponds to the current region and mask. Skipping
        // it on any path leaves a stale value that defeats the next
        // unchanged-content comparison.
        const uint64_t contentHash = hashRegion(view, region, settings.maskedWindow);
        if (!settingsChanged && contentHash == lastContentHash) {
            continue;
        }
        lastContentHash = contentHash;

        if (settingsChanged) {
            vectorscope.configure(settings.vectorscope);
            waveform.configure(settings.waveform);
            WaveformSettings parade = settings.waveform;
            parade.mode = WaveformMode::RgbParade;
            waveformParade.configure(parade);
            histogram.configure(settings.histogram);
        }

        // Only the scopes on screen cost anything; a disabled scope's
        // image simply goes stale and the UI never draws it.
        const uint32_t enabled = settings.enabledScopes;
        const auto started = std::chrono::steady_clock::now();

        if (enabled & ScopeVectorscope) {
            vectorscope.accumulate(view, region);
        }
        if (enabled & ScopeWaveform) {
            waveform.accumulate(view, region);
        }
        if (enabled & ScopeWaveformParade) {
            waveformParade.accumulate(view, region);
        }
        if (enabled & ScopeHistogram) {
            histogram.accumulate(view, region);
        }
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (newFrame) {
            ++framesProcessed;
        }

        std::lock_guard lock(m_outputMutex);
        if (enabled & ScopeVectorscope) {
            m_output.vectorscopeImage = vectorscope.image();
        }
        if (enabled & ScopeWaveform) {
            m_output.waveformImage = waveform.image();
        }
        if (enabled & ScopeWaveformParade) {
            m_output.waveformParadeImage = waveformParade.image();
        }
        if (enabled & ScopeHistogram) {
            m_output.histogramImage = histogram.image();
            m_output.histogramOutline = histogram.outlineHeights();
        }
        m_output.accumulateMilliseconds = elapsedMs;
        m_output.framesProcessed = framesProcessed;
        ++m_output.version;
    }
}

}  // namespace sidescopes
