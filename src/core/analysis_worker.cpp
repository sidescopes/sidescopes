#include "core/analysis_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

uint64_t AnalysisWorker::consumedFrameSequence() const
{
    return m_consumedSequence.load(std::memory_order_relaxed);
}

std::vector<SsParamValue> assembleScopeParams(const std::map<std::string, double>& values,
                                              const SsScopeDescriptor& descriptor)
{
    std::vector<SsParamValue> assembled;
    for (uint32_t index = 0; index < descriptor.param_count; ++index) {
        const char* key = descriptor.params[index].key;
        const auto value = values.find(key);
        if (value != values.end()) {
            assembled.push_back(SsParamValue{key, value->second});
        }
    }

    return assembled;
}

namespace {

// One built-in scope the worker owns on its thread, plus the extensions the
// host drives it through. Identity is the module id; nothing about a scope's
// meaning is special-cased here.
struct WorkerScope
{
    std::string id;
    const SsScopeDescriptor* descriptor = nullptr;
    ScopeInstance instance;
    const SsAdaptiveImageExtension* adaptive = nullptr;
    const SsOutlineExtension* outline = nullptr;
};

// One instance of every registered built-in scope, created on the calling
// (worker) thread, which therefore owns them.
std::vector<WorkerScope> makeWorkerScopes()
{
    std::vector<WorkerScope> scopes;
    const ModuleRegistry& registry = builtinModules();
    for (const RegisteredScope& registered : registry.scopes()) {
        WorkerScope scope;
        scope.id = registered.descriptor->id;
        scope.descriptor = registered.descriptor;
        scope.instance = registry.createInstance(scope.id);
        if (scope.instance.valid()) {
            scope.adaptive =
                static_cast<const SsAdaptiveImageExtension*>(scope.instance.getExtension(AdaptiveImageExtension));
            scope.outline = static_cast<const SsOutlineExtension*>(scope.instance.getExtension(OutlineExtension));
        }
        scopes.push_back(std::move(scope));
    }
    return scopes;
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

// Pushes each scope's parameters and display size from the settings map into
// its module instance. A value is applied only when the settings name a key
// the scope's descriptor declares, and each SsParamValue borrows the
// descriptor's key pointer - module-owned and stable to deinit - never a
// std::string from the settings map. Results are best-effort: a module that
// fails to configure keeps its last image, which the UI simply stops
// advancing. Runs only on a settings change, so the temporary key lookups
// never touch the per-frame path.
void configureScopes(std::vector<WorkerScope>& scopes, const AnalysisSettings& settings)
{
    for (WorkerScope& scope : scopes) {
        if (!scope.instance.valid()) {
            continue;
        }

        std::vector<SsParamValue> values;
        const auto params = settings.scopeParams.find(scope.id);
        if (params != settings.scopeParams.end() && scope.descriptor) {
            values = assembleScopeParams(params->second, *scope.descriptor);
        }
        (void)scope.instance.configure(values);

        const auto size = settings.imageSizes.find(scope.id);
        if (scope.adaptive && size != settings.imageSizes.end()) {
            scope.adaptive->setImageSize(scope.instance.raw(), size->second.first, size->second.second);
        }
    }
}

// Runs each enabled scope over the region, returning the wall time the pass
// took. Only the scopes on screen cost anything; a disabled scope's image
// simply goes stale and the UI never draws it.
double accumulateScopes(std::vector<WorkerScope>& scopes, const SsFrameView& frame, const SsRect& region,
                        const std::set<std::string>& enabled)
{
    const auto started = std::chrono::steady_clock::now();
    for (WorkerScope& scope : scopes) {
        if (scope.instance.valid() && enabled.count(scope.id) != 0) {
            (void)scope.instance.accumulate(frame, region);
        }
    }

    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

// Copies each enabled scope's freshly accumulated image into @p output, keyed
// by id, plus the outline of whichever scope exports one. The image map is
// never cleared: an existing entry reuses its rgba buffer, so the steady
// state allocates nothing. The caller holds the output lock.
void writeOutput(AnalysisWorker::Output& output, std::vector<WorkerScope>& scopes, const std::set<std::string>& enabled,
                 double elapsedMs, uint64_t framesProcessed)
{
    for (WorkerScope& scope : scopes) {
        if (!scope.instance.valid() || enabled.count(scope.id) == 0) {
            continue;
        }

        copyImage(scope.instance.image(), output.images[scope.id]);
        if (scope.outline) {
            output.histogramOutline.resize(scope.outline->heights(scope.instance.raw(), nullptr, 0));
            scope.outline->heights(scope.instance.raw(), output.histogramOutline.data(),
                                   static_cast<uint32_t>(output.histogramOutline.size()));
        }
    }

    output.accumulateMilliseconds = elapsedMs;
    output.framesProcessed = framesProcessed;
    ++output.version;
}

}  // namespace

bool AnalysisWorker::takeLatestFrame()
{
    auto frame = m_mailbox.takeLatest(std::chrono::milliseconds(100));
    if (!frame) {
        return false;
    }

    std::lock_guard lock(m_frameMutex);
    if (m_hasFrame) {
        m_mailbox.returnStorage(std::move(m_latestFrame));
    }
    m_latestFrame = std::move(*frame);
    m_hasFrame = true;
    m_consumedSequence.store(m_latestFrame.sequence, std::memory_order_relaxed);

    return true;
}

bool AnalysisWorker::syncSettings(AnalysisSettings& settings, uint64_t& seenSettingsVersion)
{
    std::lock_guard lock(m_settingsMutex);
    if (m_settingsVersion == seenSettingsVersion) {
        return false;
    }
    settings = m_settings;
    seenSettingsVersion = m_settingsVersion;

    return true;
}

bool AnalysisWorker::hasWork(bool newFrame, bool settingsChanged) const
{
    std::lock_guard lock(m_frameMutex);

    return m_hasFrame && (newFrame || settingsChanged);
}

void AnalysisWorker::run()
{
    // Instances are created on this thread, which therefore owns them.
    std::vector<WorkerScope> scopes = makeWorkerScopes();
    std::set<std::string> enabledScopes;
    AnalysisSettings settings;
    uint64_t seenSettingsVersion = 0;
    uint64_t lastContentHash = 0;
    uint64_t framesProcessed = 0;

    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        const bool newFrame = takeLatestFrame();
        const bool settingsChanged = syncSettings(settings, seenSettingsVersion);
        if (!hasWork(newFrame, settingsChanged)) {
            continue;
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
            configureScopes(scopes, settings);
            // Rebuild the enabled-id lookup once per settings change, not per
            // scope per pass.
            enabledScopes = std::set<std::string>(settings.enabledScopes.begin(), settings.enabledScopes.end());
        }

        const SsFrameView boundaryFrame = toBoundaryFrame(view);
        const SsRect boundaryRegion{region.x, region.y, region.width, region.height};
        const double elapsedMs = accumulateScopes(scopes, boundaryFrame, boundaryRegion, enabledScopes);
        if (newFrame) {
            ++framesProcessed;
        }

        std::lock_guard lock(m_outputMutex);
        writeOutput(m_output, scopes, enabledScopes, elapsedMs, framesProcessed);
    }
}

}  // namespace sidescopes
