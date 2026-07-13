#include "core/analysis_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/marker_smoother.h"
#include "core/region_hash.h"
#include "modules/module_registry.h"

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

namespace {

// The worker speaks to every scope through the module boundary. The
// bridge below translates the typed settings structs into the modules'
// parameter vocabulary; it collapses when the registry replaces the
// settings structs in the next phase.
struct WorkerScope
{
    uint32_t bit;
    ScopeInstance instance;
    const SsAdaptiveImageExtension* adaptive = nullptr;
    const SsOutlineExtension* outline = nullptr;
};

WorkerScope makeWorkerScope(uint32_t bit, const char* id)
{
    WorkerScope scope{bit, builtinModules().createInstance(id)};
    if (scope.instance.valid()) {
        scope.adaptive =
            static_cast<const SsAdaptiveImageExtension*>(scope.instance.getExtension(AdaptiveImageExtension));
        scope.outline = static_cast<const SsOutlineExtension*>(scope.instance.getExtension(OutlineExtension));
    }
    return scope;
}

SsFrameView toBoundaryFrame(const FrameView& view)
{
    return SsFrameView{view.bgra,
                       view.strideBytes,
                       view.width,
                       view.height,
                       view.colorSpace == ColorSpaceHint::Srgb ? SS_COLOR_SPACE_SRGB : SS_COLOR_SPACE_UNKNOWN,
                       view.sequence};
}

void copyImage(const SsImageView& view, ScopeImage& image)
{
    image.width = view.width;
    image.height = view.height;
    image.sequence = view.sequence;
    const std::size_t bytes = static_cast<std::size_t>(view.width) * static_cast<std::size_t>(view.height) * 4;
    image.rgba.assign(view.rgba, view.rgba + bytes);
}

double choiceOfWaveformMode(WaveformMode mode)
{
    switch (mode) {
    case WaveformMode::Luma:
        return 1.0;
    case WaveformMode::ColoredLuma:
        return 2.0;
    default:
        return 0.0;  // Rgb; legacy combined modes read as RGB
    }
}

}  // namespace

void AnalysisWorker::run()
{
    // Instances are created on this thread, which therefore owns them.
    WorkerScope vectorscope = makeWorkerScope(ScopeVectorscope, "org.sidescopes.vectorscope");
    WorkerScope waveform = makeWorkerScope(ScopeWaveform, "org.sidescopes.waveform");
    WorkerScope parade = makeWorkerScope(ScopeWaveformParade, "org.sidescopes.parade");
    WorkerScope histogram = makeWorkerScope(ScopeHistogram, "org.sidescopes.histogram");
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
            vectorscope.instance.configure(
                {{"gain", settings.vectorscope.gain},
                 {"stride", static_cast<double>(settings.vectorscope.samplingStride)},
                 {"matrix", settings.vectorscope.matrix == ChromaMatrix::Bt709 ? 1.0 : 0.0},
                 {"response", settings.vectorscope.response == TraceResponse::Linear ? 1.0 : 0.0}});
            if (vectorscope.adaptive) {
                vectorscope.adaptive->setImageSize(vectorscope.instance.raw(), settings.vectorscope.size,
                                                   settings.vectorscope.size);
            }
            const std::vector<SsParamValue> waveformValues{
                {"gain", settings.waveform.gain},
                {"stride", static_cast<double>(settings.waveform.samplingStride)},
                {"mode", choiceOfWaveformMode(settings.waveform.mode)}};
            waveform.instance.configure(waveformValues);
            if (waveform.adaptive) {
                waveform.adaptive->setImageSize(waveform.instance.raw(), settings.waveform.columns,
                                                settings.waveform.imageHeight);
            }
            parade.instance.configure(
                {{"gain", settings.waveform.gain}, {"stride", static_cast<double>(settings.waveform.samplingStride)}});
            if (parade.adaptive) {
                parade.adaptive->setImageSize(parade.instance.raw(), settings.waveform.columns,
                                              settings.waveform.imageHeight);
            }
            histogram.instance.configure(
                {{"stride", static_cast<double>(settings.histogram.samplingStride)},
                 {"style", settings.histogram.style == HistogramStyle::PerChannel ? 0.0 : 1.0}});
            if (histogram.adaptive) {
                histogram.adaptive->setImageSize(histogram.instance.raw(), settings.histogram.imageWidth,
                                                 settings.histogram.imageHeight);
            }
        }

        // Only the scopes on screen cost anything; a disabled scope's
        // image simply goes stale and the UI never draws it.
        const uint32_t enabled = settings.enabledScopes;
        const SsFrameView boundaryFrame = toBoundaryFrame(view);
        const SsRect boundaryRegion{region.x, region.y, region.width, region.height};
        const auto started = std::chrono::steady_clock::now();

        for (WorkerScope* scope : {&vectorscope, &waveform, &parade, &histogram}) {
            if ((enabled & scope->bit) && scope->instance.valid()) {
                scope->instance.accumulate(boundaryFrame, boundaryRegion);
            }
        }
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (newFrame) {
            ++framesProcessed;
        }

        std::lock_guard lock(m_outputMutex);
        if (enabled & ScopeVectorscope) {
            copyImage(vectorscope.instance.image(), m_output.vectorscopeImage);
        }
        if (enabled & ScopeWaveform) {
            copyImage(waveform.instance.image(), m_output.waveformImage);
        }
        if (enabled & ScopeWaveformParade) {
            copyImage(parade.instance.image(), m_output.waveformParadeImage);
        }
        if (enabled & ScopeHistogram) {
            copyImage(histogram.instance.image(), m_output.histogramImage);
            if (histogram.outline) {
                m_output.histogramOutline.resize(histogram.outline->heights(histogram.instance.raw(), nullptr, 0));
                histogram.outline->heights(histogram.instance.raw(), m_output.histogramOutline.data(),
                                           static_cast<uint32_t>(m_output.histogramOutline.size()));
            }
        }
        m_output.accumulateMilliseconds = elapsedMs;
        m_output.framesProcessed = framesProcessed;
        ++m_output.version;
    }
}

}  // namespace sidescopes
