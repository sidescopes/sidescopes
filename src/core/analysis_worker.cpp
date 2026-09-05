#include "core/analysis_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/heap.h"
#include "core/marker_smoother.h"
#include "core/region_hash.h"
#include "modules/module_registry.h"

namespace sidescopes {
namespace {
// Withdrawing a partial copy must itself work without allocating. Version and
// progress belong to the caller: publication advances them, a failed fetch does
// not consume the version it still owes the interface.
void clearOutput(AnalysisWorker::Output& output, uint64_t framesProcessed, uint64_t version)
{
    output.images.clear();
    output.outlines.clear();
    output.accumulateMilliseconds = 0.0;
    output.framesProcessed = framesProcessed;
    output.version = version;
}
}  // namespace

IntRect RegionOfInterest::toPixels(int frameWidth, int frameHeight) const
{
    if (frameWidth <= 0 || frameHeight <= 0 || !std::isfinite(leftPercent) || !std::isfinite(topPercent) ||
        !std::isfinite(rightPercent) || !std::isfinite(bottomPercent)) {
        return {};
    }
    // Edges round INWARD: truncation used to include the pixel row just
    // outside the region at some fractional positions, and the region
    // border's own bright ring lives exactly there - it flickered into
    // the waveform as a phantom line near the top. A boundary pixel
    // belongs to the sample only when it is entirely inside.
    const auto floorEdge = [](double percent, int extent) {
        return static_cast<int>(std::floor(std::clamp(percent, 0.0, 100.0) * extent / 100.0));
    };
    const auto ceilEdge = [](double percent, int extent) {
        return static_cast<int>(std::ceil(std::clamp(percent, 0.0, 100.0) * extent / 100.0));
    };
    const int left = ceilEdge(leftPercent, frameWidth);
    const int top = ceilEdge(topPercent, frameHeight);
    const int right = std::max(left, floorEdge(rightPercent, frameWidth));
    const int bottom = std::max(top, floorEdge(bottomPercent, frameHeight));
    return IntRect{left, top, right - left, bottom - top};
}

double scopeParam(const AnalysisSettings& settings, std::string_view id, std::string_view key, double fallback)
{
    const auto scope = settings.scopeParams.find(std::string{id});
    if (scope == settings.scopeParams.end()) {
        return fallback;
    }
    const auto value = scope->second.find(std::string{key});

    return value != scope->second.end() ? value->second : fallback;
}

AnalysisWorker::AnalysisWorker(FrameMailbox& mailbox)
    : AnalysisWorker(mailbox, builtinModules())
{
}

AnalysisWorker::AnalysisWorker(FrameMailbox& mailbox, const ModuleRegistry& registry)
    : m_mailbox(mailbox),
      m_registry(registry)
{
}

AnalysisWorker::~AnalysisWorker()
{
    stop();
}

void AnalysisWorker::start()
{
    if (m_thread.joinable() || m_inlinePass) {
        return;
    }
    m_stopRequested.store(false);
    m_thread = std::thread(&AnalysisWorker::run, this);
}

void AnalysisWorker::stop()
{
    m_inlinePass.reset();
    if (!m_thread.joinable()) {
        return;
    }
    m_stopRequested.store(true);
    m_mailbox.nudge();
    m_thread.join();
}

void AnalysisWorker::updateSettings(const AnalysisSettings& settings)
{
    // Copy before committing: container copy assignment can throw after it
    // has already changed part of the accepted settings.
    AnalysisSettings pending = settings;
    {
        std::lock_guard lock(m_settingsMutex);
        m_settings = std::move(pending);
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
    try {
        output = m_output;
    } catch (const std::bad_alloc&) {
        clearOutput(output, 0, lastSeenVersion);
        diagEmit(DiagChannel::Perf, "analysis fetch allocation failed; retrying the published output");
        // The caller must redraw the withdrawal and keep retrying, even if
        // no new frame or settings change will produce another publication.
        return true;
    }
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
    return m_consumedSequence.load(std::memory_order_acquire);
}

std::vector<SsParamValue> assembleScopeParams(const std::map<std::string, double>& values,
                                              const SsScopeDescriptor& descriptor)
{
    std::vector<SsParamValue> assembled;
    for (uint32_t index = 0; index < descriptor.param_count; ++index) {
        const SsParamInfo& parameter = descriptor.params[index];
        const char* key = parameter.key;
        const auto value = values.find(key);
        const double configured =
            value != values.end() && std::isfinite(value->second) ? value->second : parameter.default_value;
        assembled.push_back(SsParamValue{key, configured});
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
    const SsSampleThinningExtension* thinning = nullptr;
    const SsOutlineExtension* outline = nullptr;
    bool configured = false;
    bool accumulated = false;
};

// A record for every registered built-in scope, with no instance yet: an
// instance is what allocates a scope's bin and plane sets, and a scope nobody is
// looking at should not own any. Records are cheap and their order is the
// registry's, so the per-pass loops are unchanged.
std::vector<WorkerScope> makeWorkerScopes(const ModuleRegistry& registry)
{
    std::vector<WorkerScope> scopes;
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
void syncScopeInstances(std::vector<WorkerScope>& scopes, const std::vector<std::string>& enabled,
                        const ModuleRegistry& registry)
{
    for (WorkerScope& scope : scopes) {
        const bool wanted = std::find(enabled.begin(), enabled.end(), scope.id) != enabled.end();
        if (wanted == scope.instance.valid()) {
            continue;
        }
        if (!wanted) {
            scope.instance = ScopeInstance{};
            scope.adaptive = nullptr;
            scope.thinning = nullptr;
            scope.outline = nullptr;
            continue;
        }
        scope.configured = false;
        scope.accumulated = false;
        scope.instance = registry.createInstance(scope.id);
        if (scope.instance.valid()) {
            scope.adaptive =
                static_cast<const SsAdaptiveImageExtension*>(scope.instance.getExtension(AdaptiveImageExtension));
            scope.thinning =
                static_cast<const SsSampleThinningExtension*>(scope.instance.getExtension(SampleThinningExtension));
            scope.outline = static_cast<const SsOutlineExtension*>(scope.instance.getExtension(OutlineExtension));
        }
    }
}

SsFrameView toBoundaryFrame(const FrameView& view)
{
    return SsFrameView{view.pixels,
                       view.strideBytes,
                       view.width,
                       view.height,
                       view.colorSpace == ColorSpaceHint::Srgb ? SS_COLOR_SPACE_SRGB : SS_COLOR_SPACE_UNKNOWN,
                       view.sequence,
                       view.format == PixelFormat::Argb2101010 ? SS_PIXEL_FORMAT_ARGB2101010 : SS_PIXEL_FORMAT_BGRA8};
}

bool copyImage(const SsImageView& view, ScopeImage& image)
{
    if (view.width <= 0 || view.height <= 0 || !view.rgba) {
        return false;
    }
    const auto width = static_cast<std::size_t>(view.width);
    const auto height = static_cast<std::size_t>(view.height);
    if (width > image.rgba.max_size() / 4 / height) {
        return false;
    }
    image.rgba.assign(view.rgba, view.rgba + width * height * 4);
    image.width = view.width;
    image.height = view.height;
    image.sequence = view.sequence;
    return true;
}

// Applies only declared parameters, filling absent values from the descriptor.
// Failed configuration is retried before the next accumulate, so a temporary
// allocation failure does not strand an otherwise usable module.
void configureScope(WorkerScope& scope, const AnalysisSettings& settings)
{
    static const std::map<std::string, double> noParameters;
    const auto params = settings.scopeParams.find(scope.id);
    const auto values =
        assembleScopeParams(params != settings.scopeParams.end() ? params->second : noParameters, *scope.descriptor);
    scope.configured = scope.instance.configure(values);
    if (!scope.configured) {
        return;
    }
    const auto size = settings.imageSizes.find(scope.id);
    if (scope.adaptive && size != settings.imageSizes.end()) {
        scope.adaptive->setImageSize(scope.instance.raw(), size->second.first, size->second.second);
    }
    if (scope.thinning) {
        scope.thinning->setSampleThinning(scope.instance.raw(), settings.sampleThinning);
    }
}

void retryMissingInstances(std::vector<WorkerScope>& scopes, const AnalysisSettings& settings,
                           const ModuleRegistry& registry)
{
    const bool missing = std::any_of(scopes.begin(), scopes.end(), [&](const WorkerScope& scope) {
        return !scope.instance.valid() && std::find(settings.enabledScopes.begin(), settings.enabledScopes.end(),
                                                    scope.id) != settings.enabledScopes.end();
    });
    if (missing) {
        syncScopeInstances(scopes, settings.enabledScopes, registry);
    }
}

void configureScopes(std::vector<WorkerScope>& scopes, const AnalysisSettings& settings)
{
    for (WorkerScope& scope : scopes) {
        if (scope.instance.valid()) {
            configureScope(scope, settings);
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
                   std::set<std::string>& enabledScopes, const ModuleRegistry& registry)
{
    const std::vector<std::string> enabled = settings.region ? settings.enabledScopes : std::vector<std::string>{};
    syncScopeInstances(scopes, enabled, registry);
    configureScopes(scopes, settings);
    enabledScopes = std::set<std::string>(enabled.begin(), enabled.end());
}

// The part of @p view the scopes should read for @p region, in the frame's own
// pixels - or nothing when the frame cannot answer for that region at all.
//
// The region is measured against the DISPLAY and then moved into this frame's
// pixels, so a narrowed capture analyses the same content rather than the same
// percentages of a smaller frame. A frame carrying only part of the region
// answers for nowhere in particular: the crop trails the region it was made for
// by the reconfiguration's own latency, and clipping the region to the overlap
// would publish a reading of a fraction of it as if it were the whole.
std::optional<IntRect> regionInFrame(const FrameView& view, const RegionOfInterest& region)
{
    const IntRect onDisplay =
        region.toPixels(view.displayWidth(), view.displayHeight()).clampedTo(view.displayWidth(), view.displayHeight());
    if (!view.carries(onDisplay)) {
        return std::nullopt;
    }

    return view.fromDisplay(onDisplay).clampedTo(view.width, view.height);
}

// Runs each enabled scope over the region, returning the wall time the pass
// took. Only the scopes on screen cost anything; a disabled scope's image
// simply goes stale and the UI never draws it.
double accumulateScopes(std::vector<WorkerScope>& scopes, const SsFrameView& frame, const SsRect& region,
                        const std::set<std::string>& enabled, const AnalysisSettings& settings)
{
    const auto started = std::chrono::steady_clock::now();
    for (WorkerScope& scope : scopes) {
        if (scope.instance.valid() && enabled.count(scope.id) != 0) {
            if (!scope.configured) {
                configureScope(scope, settings);
            }
            scope.accumulated = scope.configured && scope.instance.accumulate(frame, region);
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
        if (enabled.count(scope.id) == 0) {
            continue;
        }

        ScopeImage& image = output.images[scope.id];
        if (!scope.instance.valid() || !scope.accumulated || !copyImage(scope.instance.image(), image)) {
            image = {};
            output.outlines.erase(scope.id);
            scope.accumulated = false;
            continue;
        }
        if (scope.outline) {
            std::vector<float>& outline = output.outlines[scope.id];
            outline.resize(scope.outline->heights(scope.instance.raw(), nullptr, 0));
            scope.outline->heights(scope.instance.raw(), outline.data(), static_cast<uint32_t>(outline.size()));
        }
    }

    output.accumulateMilliseconds = elapsedMs;
    output.framesProcessed = framesProcessed;
    ++output.version;
}

}  // namespace

bool AnalysisWorker::takeLatestFrame(std::chrono::milliseconds wait)
{
    auto frame = m_mailbox.takeLatest(wait);
    if (m_releaseFrame.exchange(false, std::memory_order_relaxed)) {
        // Capture has stopped, so anything that arrived alongside the request
        // is the last frame in flight and is let go with the rest. The content
        // hash is deliberately left standing: a screen that has not changed
        // over the pause deserves the same skip it would get without one.
        dropFrame();
        // The allocator caches what it has just been handed back, so without
        // this the pause frees the frames on paper and the process keeps every
        // page of them.
        releaseFreeHeap();

        return false;
    }
    if (!frame) {
        return false;
    }

    std::lock_guard lock(m_frameMutex);
    if (m_hasFrame) {
        m_mailbox.returnStorage(std::move(m_latestFrame));
    }
    m_latestFrame = std::move(*frame);
    m_hasFrame = true;

    return true;
}

void AnalysisWorker::releaseFrame()
{
    m_releaseFrame.store(true, std::memory_order_relaxed);
    // The take is what the worker is sitting in; end its wait so the frame is
    // let go now rather than at the end of the timeout.
    m_mailbox.nudge();
}

void AnalysisWorker::dropFrame()
{
    std::lock_guard lock(m_frameMutex);
    m_hasFrame = false;
    m_latestFrame = FrameBuffer{};
    m_consumedSequence.store(0, std::memory_order_relaxed);
}

void AnalysisWorker::hold(bool held)
{
    if (m_held.load(std::memory_order_relaxed) == held) {
        return;
    }
    m_held.store(held, std::memory_order_relaxed);
    if (held) {
        return;
    }

    // A release is a reason to run in itself. Every settings change that
    // arrived during the hold was applied the moment it was seen but never
    // acted on, so nothing else records that the images on screen answer for
    // somewhere the region has since left - and with the content unchanged and
    // no new settings, the two gates below would both skip. Advancing the
    // version is what the settings path already means by "recompute"; the flag
    // is cleared first so a held pass can never consume it.
    {
        std::lock_guard lock(m_settingsMutex);
        ++m_settingsVersion;
    }
    m_mailbox.nudge();
}

bool AnalysisWorker::held() const
{
    return m_held.load(std::memory_order_relaxed);
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
// because the colour under the pointer is read from it. A hold is the same
// shape of answer for a different reason: the region is in transit, so there
// is nothing worth reading off it until it lands.
bool AnalysisWorker::hasWork(bool newFrame, bool settingsChanged) const
{
    if (m_held.load(std::memory_order_relaxed)) {
        return false;
    }
    std::lock_guard lock(m_frameMutex);

    return m_hasFrame && (newFrame || settingsChanged);
}

// The state one pass hands to the next. Its owner is whichever thread runs
// the passes, and no other touches it.
struct AnalysisWorker::Pass
{
    std::vector<WorkerScope> scopes;
    bool scopesInitialized = false;
    std::set<std::string> enabledScopes;
    AnalysisSettings settings;
    uint64_t seenSettingsVersion = 0;
    std::optional<uint64_t> lastContentHash;
    uint64_t framesProcessed = 0;
};

void AnalysisWorker::run()
{
    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        try {
            // Even an empty map or set can allocate on some standard
            // libraries. Keep the pass constructor inside recovery too.
            Pass pass;
            while (!m_stopRequested.load(std::memory_order_relaxed)) {
                runPass(pass, std::chrono::milliseconds(100));
            }
        } catch (const std::bad_alloc&) {
            // Keep draining frames and use the normal mailbox wait so a
            // sustained startup failure neither blocks capture nor spins.
            const bool newFrame = takeLatestFrame(std::chrono::milliseconds(100));
            publishAllocationFailure(0);
            if (newFrame) {
                m_consumedSequence.store(m_latestFrame.sequence, std::memory_order_release);
            }
        }
    }
}

void AnalysisWorker::startInline()
{
    if (m_thread.joinable() || m_inlinePass) {
        return;
    }
    m_stopRequested.store(false);
    m_inlinePass = std::make_unique<Pass>();
}

void AnalysisWorker::pump()
{
    if (!m_inlinePass) {
        return;
    }
    // Zero wait: the caller is a frame loop and must not be blocked. A pass
    // with no frame to take simply does nothing and the next frame calls
    // again.
    runPass(*m_inlinePass, std::chrono::milliseconds(0));
}

void AnalysisWorker::runPass(Pass& pass, std::chrono::milliseconds wait)
{
    const bool newFrame = takeLatestFrame(wait);
    try {
        if (!pass.scopesInitialized) {
            pass.scopes = makeWorkerScopes(m_registry);
            pass.scopesInitialized = true;
        }
        analyzeLatestFrame(pass, newFrame);
    } catch (const std::bad_alloc&) {
        // A failed settings application may have reconfigured only some
        // scopes. Withdraw the old reading until a complete pass succeeds.
        pass.lastContentHash.reset();
        publishAllocationFailure(pass.framesProcessed);
    }
    if (newFrame) {
        // Publish completion after every analysis/skip path, so callers can
        // distinguish a consumed frame from one still being processed.
        m_consumedSequence.store(m_latestFrame.sequence, std::memory_order_release);
    }
}

void AnalysisWorker::analyzeLatestFrame(Pass& pass, bool newFrame)
{
    std::vector<WorkerScope>& scopes = pass.scopes;
    std::set<std::string>& enabledScopes = pass.enabledScopes;
    AnalysisSettings& settings = pass.settings;
    uint64_t candidateSettingsVersion = pass.seenSettingsVersion;
    const bool settingsChanged = syncSettings(settings, candidateSettingsVersion);
    // A settings change is applied the moment it is seen, before the work
    // gate below. Consumed on a frameless pass it would otherwise advance
    // the settings version without reconfiguring the scopes, so the first
    // frame after it would run the default - on startup, empty - scope set
    // and publish an output with no images.
    if (settingsChanged) {
        applySettings(scopes, settings, enabledScopes, m_registry);
        // Applying settings also allocates. Only a complete application may
        // consume this version, so a failed partial application is retried.
        pass.seenSettingsVersion = candidateSettingsVersion;
    }
    if (!settings.region || !hasWork(newFrame, settingsChanged)) {
        return;
    }

    // Reading the frame without the lock is safe: this thread is the
    // only writer, and readers on other threads take the mutex only for
    // the brief sampling reads that tolerate the previous frame.
    const FrameView view = m_latestFrame.view();
    // The last images stand while the frame cannot answer for the region.
    const std::optional<IntRect> region = regionInFrame(view, *settings.region);
    if (!region) {
        return;
    }

    // The hash is computed on every pass — including settings-only ones —
    // so it always corresponds to the current region and mask. Skipping
    // it on any path leaves a stale value that defeats the next
    // unchanged-content comparison.
    const uint64_t contentHash = hashRegion(view, *region, view.fromDisplay(settings.maskedWindow));
    if (!settingsChanged && contentHash == pass.lastContentHash) {
        return;
    }
    pass.lastContentHash = contentHash;

    retryMissingInstances(scopes, settings, m_registry);
    const SsFrameView boundaryFrame = toBoundaryFrame(view);
    const SsRect boundaryRegion{region->x, region->y, region->width, region->height};
    const double elapsedMs = accumulateScopes(scopes, boundaryFrame, boundaryRegion, enabledScopes, settings);
    if (newFrame) {
        ++pass.framesProcessed;
    }

    publishOutput(pass, elapsedMs);
}

void AnalysisWorker::publishOutput(Pass& pass, double elapsedMs)
{
    std::vector<WorkerScope>& scopes = pass.scopes;
    const std::set<std::string>& enabledScopes = pass.enabledScopes;
    {
        std::lock_guard lock(m_outputMutex);
        try {
            writeOutput(m_output, scopes, enabledScopes, elapsedMs, pass.framesProcessed);
        } catch (const std::bad_alloc&) {
            // Some images may already have been copied. Withdraw the whole
            // partial result before releasing the lock, then retry next frame.
            clearOutput(m_output, pass.framesProcessed, m_output.version + 1);
            pass.lastContentHash.reset();
            diagEmit(DiagChannel::Perf, "analysis output allocation failed; retrying on the next frame");
        }
    }
    if (std::any_of(scopes.begin(), scopes.end(), [&](const WorkerScope& scope) {
            return enabledScopes.count(scope.id) != 0 && (!scope.instance.valid() || !scope.accumulated);
        })) {
        pass.lastContentHash.reset();
    }
    notifyOutput();
}

void AnalysisWorker::notifyOutput() const
{
    try {
        if (m_outputCallback) {
            m_outputCallback();
        }
    } catch (const std::bad_alloc&) {
        // Notifications can also run from an allocation-recovery path. They
        // must not escape that catch or invalidate already published output.
        diagEmit(DiagChannel::Perf, "analysis notification allocation failed; output remains available");
    }
}

void AnalysisWorker::publishAllocationFailure(uint64_t framesProcessed)
{
    {
        std::lock_guard lock(m_outputMutex);
        clearOutput(m_output, framesProcessed, m_output.version + 1);
    }
    diagEmit(DiagChannel::Perf, "analysis allocation failed; retrying on the next frame");
    notifyOutput();
}

}  // namespace sidescopes
