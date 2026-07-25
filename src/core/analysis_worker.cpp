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

std::optional<FloatColor> AnalysisWorker::sampleDisplayColor(int displayX, int displayY, int radius) const
{
    std::lock_guard lock(m_frameMutex);
    if (!m_hasFrame) {
        return std::nullopt;
    }
    const FrameView view = m_latestFrame.view();
    const IntRect point = view.fromDisplay(IntRect{displayX, displayY, 1, 1});
    if (point.x < 0 || point.x >= view.width || point.y < 0 || point.y >= view.height) {
        return std::nullopt;
    }
    return averageNeighborhood(view, point.x, point.y, radius);
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

void AnalysisWorker::setOutputCallback(std::function<void()> callback)
{
    // Set before start(), so the analysis thread never sees it change.
    m_outputCallback = std::move(callback);
}

std::optional<AnalysisWorker::FrameSize> AnalysisWorker::latestFrameSize() const
{
    std::lock_guard lock(m_frameMutex);
    if (!m_hasFrame) {
        return std::nullopt;
    }
    return FrameSize{m_latestFrame.width, m_latestFrame.height, m_latestFrame.displayWidth(),
                     m_latestFrame.displayHeight()};
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

// A record for every registered built-in scope, with no instance yet: an
// instance is what allocates a scope's bin and plane sets, and a scope nobody is
// looking at should not own any. Records are cheap and their order is the
// registry's, so the per-pass loops are unchanged.
std::vector<WorkerScope> makeWorkerScopes()
{
    std::vector<WorkerScope> scopes;
    const ModuleRegistry& registry = builtinModules();
    for (const RegisteredScope& registered : registry.scopes()) {
        WorkerScope scope;
        scope.id = registered.descriptor->id;
        scope.descriptor = registered.descriptor;
        scopes.push_back(std::move(scope));
    }

    return scopes;
}

// Gives every enabled scope an instance and takes it away from the rest, so the
// memory a scope needs is owned exactly while it is on screen. Every registered
// scope used to be instantiated at startup, which on a full registry is tens of
// megabytes of plane sets - the parade alone is a second complete waveform - paid
// by a session showing one histogram.
//
// Called on a settings change, before the scopes are configured, so a scope that
// has just appeared is instantiated in time to receive its parameters. Dropping
// an instance drops whatever it had accumulated, which is right: a scope coming
// back on screen re-reads the region from the next frame.
void syncScopeInstances(std::vector<WorkerScope>& scopes, const std::vector<std::string>& enabled)
{
    const ModuleRegistry& registry = builtinModules();
    for (WorkerScope& scope : scopes) {
        const bool wanted = std::find(enabled.begin(), enabled.end(), scope.id) != enabled.end();
        if (wanted == scope.instance.valid()) {
            continue;
        }
        if (!wanted) {
            scope.instance = ScopeInstance{};
            scope.adaptive = nullptr;
            scope.outline = nullptr;
            continue;
        }
        scope.instance = registry.createInstance(scope.id);
        if (scope.instance.valid()) {
            scope.adaptive =
                static_cast<const SsAdaptiveImageExtension*>(scope.instance.getExtension(AdaptiveImageExtension));
            scope.outline = static_cast<const SsOutlineExtension*>(scope.instance.getExtension(OutlineExtension));
        }
    }
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

// Everything a settings change has to do before any frame is analysed, in the
// order it has to happen: instances first, because a scope that has just been
// shown must exist before it can be configured or it would run its module
// defaults for a pass; then the parameters; then the enabled-id lookup the
// per-pass loop reads, rebuilt once here rather than per scope per pass.
//
// A settings change carrying no region enables nothing, so every instance -
// and the bins and planes it holds - is dropped along with the selection.
void applySettings(std::vector<WorkerScope>& scopes, const AnalysisSettings& settings,
                   std::set<std::string>& enabledScopes)
{
    const std::vector<std::string> enabled = settings.region ? settings.enabledScopes : std::vector<std::string>{};
    syncScopeInstances(scopes, enabled);
    configureScopes(scopes, settings);
    enabledScopes = std::set<std::string>(enabled.begin(), enabled.end());
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

// A pass needs a frame to read and a reason to run: a new frame, or settings
// that changed. The region it reads is the caller's own test, stated beside
// this one - a pass without a region computes nothing and publishes nothing,
// so the scopes stay as empty as they started, while the frame is still taken
// because the colour under the pointer is read from it.
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
        // A settings change is applied the moment it is seen, before the work
        // gate below. Consumed on a frameless pass it would otherwise advance
        // the settings version without reconfiguring the scopes, so the first
        // frame after it would run the default - on startup, empty - scope set
        // and publish an output with no images.
        if (settingsChanged) {
            applySettings(scopes, settings, enabledScopes);
        }
        if (!settings.region || !hasWork(newFrame, settingsChanged)) {
            continue;
        }

        // Reading the frame without the lock is safe: this thread is the
        // only writer, and readers on other threads take the mutex only for
        // the brief sampling reads that tolerate the previous frame.
        const FrameView view = m_latestFrame.view();
        // Measured against the DISPLAY, then moved into this frame's pixels:
        // a narrowed capture must analyse the same content, not the same
        // percentages of a smaller frame.
        const IntRect region = view.fromDisplay(settings.region->toPixels(view.displayWidth(), view.displayHeight()))
                                   .clampedTo(view.width, view.height);

        // The hash is computed on every pass — including settings-only ones —
        // so it always corresponds to the current region and mask. Skipping
        // it on any path leaves a stale value that defeats the next
        // unchanged-content comparison.
        const uint64_t contentHash = hashRegion(view, region, view.fromDisplay(settings.maskedWindow));
        if (!settingsChanged && contentHash == lastContentHash) {
            continue;
        }
        lastContentHash = contentHash;

        const SsFrameView boundaryFrame = toBoundaryFrame(view);
        const SsRect boundaryRegion{region.x, region.y, region.width, region.height};
        const double elapsedMs = accumulateScopes(scopes, boundaryFrame, boundaryRegion, enabledScopes);
        if (newFrame) {
            ++framesProcessed;
        }

        std::lock_guard lock(m_outputMutex);
        writeOutput(m_output, scopes, enabledScopes, elapsedMs, framesProcessed);
        if (m_outputCallback) {
            m_outputCallback();
        }
    }
}

}  // namespace sidescopes
