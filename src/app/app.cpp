// The SideScopes application shell, shared by every platform: a compact,
// always-on-top window stacking the enabled scopes. All analysis lives in
// the core library on its own thread; this file owns the interaction
// model (gestures, native menu, region selection) and preferences, while
// rendering and window chrome live behind the graphics seam.

#include "app/app.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "app/app_startup.h"
#include "app/border_label.h"
#include "app/capture_controller.h"
#include "app/color_readout.h"
#include "app/context_menu.h"
#include "app/imgui_ui.h"
#include "app/overlay_render.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/region_geometry.h"
#include "app/row_layout.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/settings_window.h"
#include "app/ui_scaling.h"
#include "app/version.h"
#include "app/window_suggestions.h"
#include "core/analysis_worker.h"
#include "core/color_lab.h"
#include "core/diagnostics.h"
#include "core/frame_mailbox.h"
#include "core/marker_smoother.h"
#include "core/preferences.h"
#include "core/region_suggestions.h"
#include "core/scopes/histogram.h"
#include "core/scopes/waveform.h"
#include "core/trace_intensity.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"
#include "platform/graphics.h"
#include "platform/native_menu.h"
#include "platform/region_selection.h"
#include "platform/screen_capture.h"
#include "sidescopes_version.h"

namespace {

// A shortcut name resolves to the ImGui key it fires on; only Escape and the
// bare letters are bindable, so anything else never matches a press.
ImGuiKey keyFor(const std::string& name)
{
    if (name == "Escape") {
        return ImGuiKey_Escape;
    }
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'A'));
    }

    return ImGuiKey_None;
}

bool shortcutPressed(const std::string& binding)
{
    const ImGuiKey key = keyFor(binding);

    return key != ImGuiKey_None && ImGui::IsKeyPressed(key, false);
}

}  // namespace

namespace sidescopes {

App::App()
    : m_worker(m_mailbox),
      m_capture(createScreenCaptureSource()),
      m_captureController(*m_capture, m_mailbox),
      m_faceLock(m_attach, m_worker, m_captureController),
      m_regionPicker(m_captureController, m_worker, *m_capture),
      m_scopeRegistry(builtinModules()),
      m_view(m_scopeRegistry)
{
    m_screenSample = std::make_shared<ScreenSample>();
}

bool App::init()
{
    diagInit();
    if (!glfwInit()) {
        return false;
    }

    const Preferences startup = loadPreferences(preferencesFilePath());
    m_versionInfo = describeVersion(SIDESCOPES_VERSION, SIDESCOPES_GIT_DESCRIBE);

    MainWindow main = createMainWindow(startup, m_versionInfo, m_callbackState);
    if (!main.window) {
        return false;
    }
    m_window = main.window;
    m_graphics = std::move(main.graphics);
    setupImGui();
    if (!m_graphics->init(m_window)) {
        ImGui::DestroyContext();
        glfwDestroyWindow(m_window);
        glfwTerminate();

        return false;
    }

    setupCapture();
    seedAnalysis(m_analysis, startup);
    setupView(startup);
    m_projectionInstances = createProjectionInstances(m_scopeRegistry);
    ScopeTextureSet textures = createScopeTextures(*m_graphics, m_scopeRegistry);
    m_scopeTextures = std::move(textures.textures);
    m_panePoints = std::move(textures.panePoints);
    m_paneIds = std::move(textures.paneIds);
    m_dividerIds = std::move(textures.dividerIds);

    m_shortcuts = startup.shortcuts;
    m_scopeShortcuts = startup.scopeShortcuts;

    m_worker.start();
    warmFaceDetection();

    observeSystemWake([this] { m_captureController.markStale(); });
    observeEscapeWithoutKeyWindow([this] { m_orphanEscape.store(true); });
    // A foreground switch reroutes the borders at the top of the next frame:
    // the flag makes the loop route on arrival, and the empty event wakes an
    // idle wait so "the next frame" is now.
    observeForegroundChanges([this] {
        m_callbackState.foregroundChanged.store(true);
        glfwPostEmptyEvent();
    });
    rememberApplicationWindow(m_graphics->nativeWindowHandle());
    m_ownPid = ownApplicationPid();

    m_lastActivity = glfwGetTime();
    syncRegionBorder();

    return true;
}

void App::run()
{
    while (!glfwWindowShouldClose(m_window)) {
        runFrame();
    }
}

void App::shutdown()
{
    // No new face-lock probe or display face scan starts once the loop is
    // done, so drain any still in flight before their targets leave scope:
    // the detached threads hold pointers into this object.
    for (;;) {
        if (!m_faceLock.probeRunning() && !m_regionPicker.scansRunning()) {
            break;
        }
        std::this_thread::yield();
    }

    persistPreferences();
    unwatchWindowMotion();
    // The observer reaches this object and posts GLFW events, so it must not
    // outlive either.
    unobserveForegroundChanges();
    hideAttachedEditDim();
    hideRegionBorder();
    m_worker.stop();
    m_capture->stop();
    m_graphics->shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

void App::setupImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is ours to persist
    ImGui::StyleColorsDark();
    applyTheme();
    m_callbackState.monospaceFont = loadInterfaceFont(m_window);
    refreshUiScale();
}

void App::applyUiScale(float scale)
{
    m_uiScale = scale;
    // Scaling an already scaled style would compound, so rebuild from the base
    // theme each time.
    applyTheme();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
}

bool App::refreshUiScale()
{
    // The OS scale is the baseline - it already carries the monitor's own
    // recommendation - and the user factor asks for more or less on top. Folding
    // them in one place keeps the startup and monitor-change sites in step, so a
    // window crossing displays never loses the preference.
    const float target = computeUiScale(m_window) * m_userUiScaleFactor;
    if (target == m_uiScale) {
        return false;
    }
    applyUiScale(target);

    return true;
}

void App::setupCapture()
{
    // The source and controller are constructed with the App; here they only
    // start capturing. The display under this window's center: full-screen
    // capture is a promise about the screen the user can see the scopes on.
    if (m_captureController.requestPermission()) {
        m_captureController.requestDisplay(displayOfWindow().value_or(0));
        m_captureController.start();
    }
}

void App::setupView(const Preferences& startup)
{
    // The file format caps its pin list at the ring's capacity; the two
    // constants sit in different layers, so the build checks they agree.
    static_assert(MaximumPins == PinBoard::Maximum);
    m_pins.restore(startup.pins, startup.pinComparator);
    m_view.restoreStack(startup.scopeStack);
    m_view.setGraticule(startup.showGraticule);
    m_view.setZoom(startup.vectorscopeZoom);
    m_view.setOrientation(orientationFromInt(startup.layoutOrientation));
    m_view.setWeights(startup.layoutWeights);
    m_presetStore.restore(startup.layoutPresets, startup.layoutActiveSlot);
    // The stored factor is cleaned to an offered step here, at the app boundary,
    // so core preferences never depend on the app's scaling policy. setupImGui
    // already applied the OS scale at the 1.0 default; fold the preference in now,
    // before the first frame.
    m_userUiScaleFactor = cleanedUiScaleFactor(startup.uiScaleFactor);
    refreshUiScale();
    // The intensity control is derived from each trace's saved gain; smoothing
    // is the host's own per-scope value, read straight from the preferences.
    const auto startupSmoothing = [&](std::string_view id, double fallback) -> float {
        const auto scope = startup.scopeParams.find(std::string{id});
        if (scope == startup.scopeParams.end()) {
            return static_cast<float>(fallback);
        }
        const auto value = scope->second.find("smoothing_ms");

        return value != scope->second.end() ? static_cast<float>(value->second) : static_cast<float>(fallback);
    };
    m_view.setIntensity(VectorscopeScopeId,
                        intensityFromTraceGain(static_cast<float>(scopeParam(VectorscopeScopeId, "gain", 3.0)),
                                               VectorscopeIntensityShift));
    m_view.setIntensity(WaveformScopeId,
                        intensityFromTraceGain(static_cast<float>(scopeParam(WaveformScopeId, "gain", 0.05))));
    m_view.setSmoothing(VectorscopeScopeId, startupSmoothing(VectorscopeScopeId, 75.0));
    m_view.setSmoothing(WaveformScopeId, startupSmoothing(WaveformScopeId, 100.0));
    m_analysis.enabledScopes = m_view.enabledScopeIds();
}

std::optional<uint32_t> App::displayOfWindow() const
{
    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(m_window, &windowX, &windowY);
    glfwGetWindowSize(m_window, &windowWidth, &windowHeight);

    return displayAtPoint(DesktopPoint{windowX + windowWidth / 2.0, windowY + windowHeight / 2.0});
}

double App::scopeParam(std::string_view id, std::string_view key, double fallback) const
{
    const auto scope = m_analysis.scopeParams.find(std::string{id});
    if (scope == m_analysis.scopeParams.end()) {
        return fallback;
    }
    const auto value = scope->second.find(std::string{key});

    return value != scope->second.end() ? value->second : fallback;
}

std::pair<int, int> App::currentSize(std::string_view id) const
{
    const auto at = m_analysis.imageSizes.find(std::string{id});

    return at != m_analysis.imageSizes.end() ? at->second : std::pair<int, int>{0, 0};
}

HistogramStyle App::currentHistogramStyle() const
{
    return scopeParam(HistogramScopeId, "style", 0.0) < 0.5 ? HistogramStyle::PerChannel : HistogramStyle::Combined;
}

void App::setWaveformGain(double gain)
{
    m_analysis.scopeParams[WaveformScopeId]["gain"] = gain;
    m_analysis.scopeParams[ParadeScopeId]["gain"] = gain;
}

const SsScopeDescriptor* App::descriptorFor(std::string_view id) const
{
    const HostScope* hostScope = m_scopeRegistry.byId(id);

    return hostScope != nullptr ? hostScope->descriptor : nullptr;
}

void App::configureProjectionInstances()
{
    // Reconfigures every projection instance from the current settings, through
    // the same assembleScopeParams the worker uses, so an overlay can never
    // disagree with its trace.
    for (auto& [id, instance] : m_projectionInstances) {
        const SsScopeDescriptor* descriptor = descriptorFor(id);
        std::vector<SsParamValue> values;
        const auto params = m_analysis.scopeParams.find(id);
        if (params != m_analysis.scopeParams.end() && descriptor != nullptr) {
            values = assembleScopeParams(params->second, *descriptor);
        }
        (void)instance.configure(values);
    }
}

const ScopeInstance* App::projectionFor(std::string_view id) const
{
    const auto at = m_projectionInstances.find(std::string{id});

    return at != m_projectionInstances.end() ? &at->second : nullptr;
}

std::string App::bindingFor(std::string_view id) const
{
    return resolveBinding(m_scopeShortcuts, m_scopeRegistry, id);
}

bool App::pinsAvailable() const
{
    // Pins mark any scope that declares itself a pin target (plus the host's
    // own color picker); without one on screen, the tool's button, menu
    // entries, and shortcuts all stand down together.
    for (const std::string& scopeId : m_view.stack()) {
        if (scopeId == ColorPickerScopeId) {
            return true;
        }
        const HostScope* hostScope = m_scopeRegistry.byId(scopeId);
        if (hostScope != nullptr && hostScope->descriptor != nullptr &&
            (hostScope->descriptor->flags & SS_SCOPE_PIN_TARGET) != 0u) {
            return true;
        }
    }

    return false;
}

const ScopeImage& App::imageForId(std::string_view id) const
{
    static const ScopeImage empty;
    const auto at = m_output.images.find(std::string{id});

    return at != m_output.images.end() ? at->second : empty;
}

ScopeTexture& App::textureForId(std::string_view id)
{
    return *m_scopeTextures.at(std::string{id});
}

void App::uploadScope(std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image)
{
    if (image.width <= 0 || image.height <= 0) {
        return;
    }
    if (image.rgba.size() < static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        return;
    }
    if (texture->width() != image.width || texture->height() != image.height) {
        texture = m_graphics->createScopeTexture(image.width, image.height);
    }
    texture->upload(image);
}

void App::uploadVisibleScopes()
{
    for (const std::string& id : m_view.stack()) {
        const auto texture = m_scopeTextures.find(id);
        if (texture == m_scopeTextures.end()) {
            continue;  // the color picker has no texture
        }
        uploadScope(texture->second, imageForId(id));
    }
}

void App::refreshActivatedScope(std::string_view id)
{
    // A scope draws the same frame it turns on, but the worker only computes
    // what is enabled, so a newly shown scope's image is stale. Turning it on
    // pushes the settings immediately and waits briefly for the recompute; on
    // timeout the stale image stands in until the recompute lands a frame later.
    if (m_scopeTextures.find(std::string{id}) == m_scopeTextures.end()) {
        return;  // the color picker asks nothing of the worker
    }
    const uint64_t staleSequence = imageForId(id).sequence;
    m_worker.updateSettings(m_analysis);
    const double deadline = glfwGetTime() + 0.08;
    while (glfwGetTime() < deadline) {
        if (m_worker.fetchOutput(m_outputVersion, m_output) && imageForId(id).sequence != staleSequence &&
            imageForId(id).width > 0) {
            uploadVisibleScopes();

            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    uploadVisibleScopes();  // timeout: a stale image beats none
}

void App::toggleScope(std::string_view id)
{
    const bool activated = m_view.toggle(id);
    m_analysis.enabledScopes = m_view.enabledScopeIds();
    if (activated) {
        refreshActivatedScope(id);
    }
    m_analysisDirty = true;
}

void App::chooseScope(std::string_view id, bool stack)
{
    const bool activated = m_view.choose(id, stack);
    m_analysis.enabledScopes = m_view.enabledScopeIds();
    if (activated) {
        refreshActivatedScope(id);
    }
    m_analysisDirty = true;
}

bool App::isFullScreen() const
{
    return m_analysis.region.leftPercent <= 0.0 && m_analysis.region.topPercent <= 0.0 &&
           m_analysis.region.rightPercent >= 100.0 && m_analysis.region.bottomPercent >= 100.0;
}

RegionKind App::regionKind() const
{
    // The active identity IS the kind: the follow step takes the attached
    // region exactly while a visible attached window holds the focus, and
    // falls back to the global region the moment it does not.
    return m_activeWindowIdentity != 0 ? RegionKind::Attached : RegionKind::Global;
}

void App::syncRegionBorder()
{
    if (m_captureController.capturedDisplay() == 0) {
        return;
    }
    // The border shows only while this application is itself visible - a
    // hidden or minimized SideScopes must not leave regions floating on
    // screen - never during a pick or window motion. What it outlines
    // follows the focus routing already folded into the analysis region: the
    // attached region on the focused attached window (label and warm dress),
    // else the plain global one. Called every frame; the platform side makes
    // the unchanged case free.
    if (m_regionPicker.active() || isFullScreen() || applicationHidden() || m_attachedWindowMoving ||
        m_faceLock.hunting() || m_faceLock.contentUnsettled(glfwGetTime()) ||
        glfwGetWindowAttrib(m_window, GLFW_ICONIFIED)) {
        hideRegionBorder();
    } else {
        const bool attached = regionKind() == RegionKind::Attached;
        if (!attached && m_captureController.capturedDisplay() != m_displayLabelId) {
            m_displayLabelId = m_captureController.capturedDisplay();
            m_displayLabel = borderLabelFrom(displayName(m_displayLabelId), "Display");
        }
        showRegionBorder(m_captureController.capturedDisplay(), m_analysis.region,
                         attached ? m_attachActiveLabel : m_displayLabel, attached);
    }
}

// Sets the capture region, skipping a no-op so the worker and border are not
// nudged for nothing.
void App::setRegion(const RegionOfInterest& region)
{
    if (region.leftPercent == m_analysis.region.leftPercent && region.topPercent == m_analysis.region.topPercent &&
        region.rightPercent == m_analysis.region.rightPercent &&
        region.bottomPercent == m_analysis.region.bottomPercent) {
        return;
    }

    m_analysis.region = region;
    m_analysisDirty = true;
    m_lastActivity = glfwGetTime();
}

void App::resetToFullScreen()
{
    // Resets all selection: a pending pick, every attached window, and the
    // global region alike. The border sync rides the analysis-dirty path.
    cancelRegionPick();
    if (m_attach.attached()) {
        m_attach.detachAll();
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
    m_globalRegion = RegionOfInterest{};
    m_analysis.region = RegionOfInterest{};
    m_analysisDirty = true;
}

void App::persistPreferences()
{
    Preferences preferences;
    // The worker's parameter map is the persisted state directly; only the
    // host-owned smoothing control is folded back in. The parade is dropped: it
    // mirrors the waveform and re-seeds on load.
    preferences.scopeParams = m_analysis.scopeParams;
    preferences.scopeParams.erase(ParadeScopeId);
    preferences.scopeParams[VectorscopeScopeId]["smoothing_ms"] = m_view.smoothing(VectorscopeScopeId);
    preferences.scopeParams[WaveformScopeId]["smoothing_ms"] = m_view.smoothing(WaveformScopeId);
    preferences.scopeStack = m_view.stackTokens();
    preferences.showGraticule = m_view.graticule();
    preferences.vectorscopeZoom = m_view.zoom();
    preferences.layoutOrientation = orientationToInt(m_view.orientation());
    preferences.layoutWeights = m_view.weightsSnapshot();
    preferences.layoutPresets = m_presetStore.all();
    preferences.layoutActiveSlot = m_presetStore.activeSlot();
    preferences.uiScaleFactor = m_userUiScaleFactor;
    preferences.shortcuts = m_shortcuts;
    preferences.scopeShortcuts = m_scopeShortcuts;
    preferences.pins = m_pins.colors();
    preferences.pinComparator = m_pins.comparator();
    glfwGetWindowPos(m_window, &preferences.windowX, &preferences.windowY);
    glfwGetWindowSize(m_window, &preferences.windowWidth, &preferences.windowHeight);
    if (!savePreferences(preferences, preferencesFilePath())) {
        std::fprintf(stderr, "sidescopes: failed to save preferences to %s\n", preferencesFilePath().c_str());
    }
}

void App::runFrame()
{
    // Frame-scoped signals start clear each iteration, exactly as fresh locals
    // would; the phase methods below fill them in.
    m_vectorscopeColor.reset();
    m_waveformColor.reset();
    m_regionPicker.clearRequest();

    pumpEvents();
    markFrameBodyStart();
    drainAsyncSignals();
    // Capture is a service that dies (lock screen, display sleep); restarting
    // it is our job.
    m_captureController.service(glfwGetTime());
    // Attached regions: observe the attached windows and route the analysis by
    // the focused window. The border reconciles here every frame in both
    // regimes, so no missed edge can strand it on screen.
    followAttachedWindow();
    syncRegionBorder();
    followWindowDisplay();
    syncUiScaleToMonitor();

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth == 0 || framebufferHeight == 0) {
        return;
    }
    if (!m_graphics->beginFrame(framebufferWidth, framebufferHeight)) {
        return;
    }

    if (m_worker.fetchOutput(m_outputVersion, m_output)) {
        uploadVisibleScopes();
        SS_DIAG(Perf, "pass analysis_ms=%.1f", m_output.accumulateMilliseconds);
        m_lastActivity = glfwGetTime();
    }
    m_frameSize = m_worker.latestFrameSize();
    publishSelfWindowMask();
    sampleCursorColor();
    updateAdaptiveDetail(framebufferWidth);

    drawFrameUi();

    ImGui::Render();
    presentFrame();

    // Second follow step, right after the vsync wait: the pre-frame geometry
    // is a frame stale by now, and a border moved from it would trail a
    // fast-dragged window visibly.
    followAttachedWindow();

    // The blocking overlay runs after the frame is submitted; capture and
    // analysis keep flowing underneath.
    applyRegionPickOutcome(m_regionPicker.openIfRequested(isFullScreen()));
    handleRegionBorderEdit();
    applyRegionPickOutcome(m_regionPicker.poll(m_frameSize, currentScreenSampleColor()));
    commitAnalysisChanges();
}

void App::markFrameBodyStart()
{
    if (diagEnabled(DiagChannel::Perf)) {
        m_frameBodyStart = std::chrono::steady_clock::now();
        m_frameBodyStamped = true;
    }
}

void App::presentFrame()
{
    // Off: a plain present. On: bracket the platform present so the frame
    // body (build, since markFrameBodyStart) and the present/vsync wait fall
    // on one line. On Windows the wait is DwmFlush's compositor tick; on
    // macOS it is the drawable present submit, the swap seam in shared code.
    // The record toggle lands mid-frame (inside the UI pass), so the first
    // present after enabling has no stamped body start - that frame logs
    // nothing rather than timing from a stale stamp.
    const bool bodyStamped = m_frameBodyStamped;
    m_frameBodyStamped = false;
    if (!diagEnabled(DiagChannel::Perf)) {
        m_graphics->endFrame();

        return;
    }
    const auto presentStart = std::chrono::steady_clock::now();
    m_graphics->endFrame();
    if (!bodyStamped) {
        return;
    }
    const auto frameEnd = std::chrono::steady_clock::now();
    const double bodyMs = std::chrono::duration<double, std::milli>(presentStart - m_frameBodyStart).count();
    const double presentMs = std::chrono::duration<double, std::milli>(frameEnd - presentStart).count();
    SS_DIAG(Perf, "frame body_ms=%.1f present_ms=%.1f", bodyMs, presentMs);
}

void App::pumpEvents()
{
    // Idle: with no new output, no cursor motion, and no interaction, wait for
    // events at a slow tick instead of spinning at refresh - in short slices
    // while windows are attached, so their motion and focus stay fresh.
    if (glfwGetTime() - m_lastActivity > 0.5) {
        if (m_attach.attached() && !m_regionPicker.active()) {
            idleWaitWatchingAttachedWindow();
        } else {
            glfwWaitEventsTimeout(0.1);
        }
    } else {
        glfwPollEvents();
    }
}

void App::drainAsyncSignals()
{
    // First of the drains, and ahead of the capture service below: the focus
    // routing is what takes a stale border down, and everything after this
    // point can stall the tick - a capture restart most of all.
    if (m_callbackState.foregroundChanged.exchange(false)) {
        SS_DIAG(Attach, "fg-event wake");
        followAttachedWindow();
        m_lastActivity = glfwGetTime();
    }
    m_regionPicker.drainFaceScans();
    if (m_callbackState.iconifyChanged.exchange(false)) {
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
    if (m_orphanEscape.exchange(false)) {
        resetToFullScreen();
        m_lastActivity = glfwGetTime();
    }
    // Keys the border panel took while it held the keyboard: Escape and the
    // shortcuts keep working right after a border interaction. Escape on the
    // border dismisses only the region it outlines - like its close button -
    // while Escape in the main window stays the full reset.
    for (const BorderKeyPress& press : drainBorderKeyPresses()) {
        if (press.escape) {
            dismissEditedBorder();
        } else {
            triggerShortcut(press.key, press.shift);
        }
        m_lastActivity = glfwGetTime();
    }
}

void App::followWindowDisplay()
{
    // With no region drawn and no window attached, capture follows the display
    // this window sits on. A drawn region or an attached window pins capture to
    // its own display regardless of the window.
    if (m_captureController.permissionGranted() && !m_captureController.dead() && !m_regionPicker.active() &&
        isFullScreen() && !m_attach.attached()) {
        const auto homeDisplay = displayOfWindow();
        if (homeDisplay && *homeDisplay != m_captureController.capturedDisplay()) {
            m_captureController.requestDisplay(*homeDisplay);
            if (m_captureController.start()) {
                m_lastActivity = glfwGetTime();
            }
        }
    }
}

void App::syncUiScaleToMonitor()
{
    // The window may have moved to a monitor with a different scale; the user
    // factor rides along through refreshUiScale.
    if (refreshUiScale()) {
        m_lastActivity = glfwGetTime();
    }
}

void App::publishSelfWindowMask()
{
    // Publish our own window rectangle (frame pixels, generous chrome margins)
    // so analysis masks it out of change detection.
    if (!m_frameSize || m_captureController.capturedDisplay() == 0) {
        return;
    }
    const auto geometry = geometryOfDisplay(m_captureController.capturedDisplay());
    if (!geometry) {
        return;
    }
    int windowX = 0, windowY = 0, windowW = 0, windowH = 0;
    glfwGetWindowPos(m_window, &windowX, &windowY);
    glfwGetWindowSize(m_window, &windowW, &windowH);
    const double scaleX = m_frameSize->width / geometry->widthPoints;
    const double scaleY = m_frameSize->height / geometry->heightPoints;
    // The chrome margins are 100%-scale units like the rest of the interface,
    // so they grow with the monitor's scale.
    const IntRect selfWindow{static_cast<int>((windowX - geometry->originX - 8.0f * m_uiScale) * scaleX),
                             static_cast<int>((windowY - geometry->originY - 42.0f * m_uiScale) * scaleY),
                             static_cast<int>((static_cast<float>(windowW) + 16.0f * m_uiScale) * scaleX),
                             static_cast<int>((static_cast<float>(windowH) + 58.0f * m_uiScale) * scaleY)};
    if (selfWindow.x != m_analysis.maskedWindow.x || selfWindow.y != m_analysis.maskedWindow.y ||
        selfWindow.width != m_analysis.maskedWindow.width || selfWindow.height != m_analysis.maskedWindow.height) {
        m_analysis.maskedWindow = selfWindow;
        m_analysisDirty = true;
    }
}

void App::sampleCursorColor()
{
    // Cursor color, smoothed per scope with its own rhythm. On the captured
    // display it reads the capture stream's frame; on every other display a
    // throttled one-shot sample keeps the readout alive even while capture is
    // paused.
    if (m_captureController.capturedDisplay() == 0) {
        return;
    }
    const auto cursor = globalCursorPosition();
    if (!cursor) {
        return;
    }
    if (std::abs(cursor->x - m_lastCursor.x) + std::abs(cursor->y - m_lastCursor.y) > 0.5) {
        m_lastCursor = *cursor;
        m_lastActivity = glfwGetTime();
    }
    std::optional<FloatColor> sampled;
    const bool onCapturedDisplay = displayAtPoint(*cursor).value_or(0) == m_captureController.capturedDisplay();
    if (onCapturedDisplay && !m_captureController.dead() && m_frameSize) {
        if (const auto geometry = geometryOfDisplay(m_captureController.capturedDisplay())) {
            const int pixelX =
                static_cast<int>((cursor->x - geometry->originX) * m_frameSize->width / geometry->widthPoints);
            const int pixelY =
                static_cast<int>((cursor->y - geometry->originY) * m_frameSize->height / geometry->heightPoints);
            sampled = m_worker.sampleFrameColor(pixelX, pixelY);
        }
    } else {
        if (glfwGetTime() > m_nextScreenSample) {
            m_nextScreenSample = glfwGetTime() + 0.05;
            auto screenSample = m_screenSample;
            sampleScreenColorAsync(*cursor, [screenSample](std::optional<FloatColor> color) {
                if (!color) {
                    return;
                }
                std::lock_guard lock(screenSample->mutex);
                screenSample->color = color;
            });
        }
        std::lock_guard lock(m_screenSample->mutex);
        sampled = m_screenSample->color;
    }
    if (sampled) {
        m_vectorscopeMarker.setTimeConstant(m_view.smoothing(VectorscopeScopeId));
        m_waveformMarker.setTimeConstant(m_view.smoothing(WaveformScopeId));
        const ImGuiIO& io = ImGui::GetIO();
        m_vectorscopeColor = m_vectorscopeMarker.update(*sampled, io.DeltaTime);
        m_waveformColor = m_waveformMarker.update(*sampled, io.DeltaTime);
    }
}

ImVec2 App::paneSizePixels(std::string_view id, float density) const
{
    const ImVec2& points = m_panePoints[static_cast<std::size_t>(m_scopeRegistry.indexOf(id))];

    return ImVec2(points.x * density, points.y * density);
}

std::pair<int, int> App::desiredWaveformSize(float density, int regionWidth) const
{
    const std::pair<int, int> waveSize = currentSize(WaveformScopeId);
    int wantColumns = waveSize.first;
    int wantHeight = waveSize.second;
    if (m_view.shows(WaveformScopeId) || m_view.shows(ParadeScopeId)) {
        const float wfWidth =
            std::max(paneSizePixels(WaveformScopeId, density).x, paneSizePixels(ParadeScopeId, density).x);
        const float wfHeight =
            std::max(paneSizePixels(WaveformScopeId, density).y, paneSizePixels(ParadeScopeId, density).y);
        wantColumns = wfWidth >= 1400.0f ? 2048 : wfWidth >= 500.0f ? 1024 : 512;
        if (regionWidth > 0) {
            wantColumns = std::min(wantColumns, regionWidth >= 2048 ? 2048 : regionWidth >= 1024 ? 1024 : 512);
        }
        wantHeight = wfHeight >= 560.0f ? 512 : WaveformLevels;
    }

    return {wantColumns, wantHeight};
}

std::pair<int, int> App::desiredHistogramSize(float density) const
{
    const std::pair<int, int> histSize = currentSize(HistogramScopeId);
    int wantHistWidth = histSize.first;
    int wantHistHeight = histSize.second;
    if (m_view.shows(HistogramScopeId)) {
        // Near one texture pixel per screen pixel keeps the outline's width even
        // on flats and steep slopes alike.
        const ImVec2 scopePane = paneSizePixels(HistogramScopeId, density);
        wantHistWidth = scopePane.x >= 1400.0f ? 2048 : scopePane.x >= 500.0f ? 1024 : 512;
        wantHistHeight = scopePane.y >= 560.0f ? 768 : 384;
    }

    return {wantHistWidth, wantHistHeight};
}

int App::desiredVectorscopeSize(float density) const
{
    int wantVectorscope = currentSize(VectorscopeScopeId).second;
    if (m_view.shows(VectorscopeScopeId)) {
        // Purely a display resolution: accumulation stays on the 256-code grid
        // and a finer image is interpolated from it, so a sparse region costs
        // nothing extra.
        const ImVec2 scopePane = paneSizePixels(VectorscopeScopeId, density);
        const float extent = std::min(scopePane.x, scopePane.y);
        wantVectorscope = extent >= 480.0f ? 512 : 256;
    }

    return wantVectorscope;
}

void App::updateAdaptiveDetail(int framebufferWidth)
{
    // Resolution follows the pane a scope actually gets, and never exceeds what
    // the region can populate; desired resolutions are debounced so a live
    // resize does not thrash engine reallocation.
    int windowW = 0;
    int windowH = 0;
    glfwGetWindowSize(m_window, &windowW, &windowH);
    const float density = windowW > 0 ? static_cast<float>(framebufferWidth) / static_cast<float>(windowW) : 1.0f;
    int regionWidth = 0;
    if (m_frameSize) {
        regionWidth = m_analysis.region.toPixels(m_frameSize->width, m_frameSize->height).width;
    }

    const std::pair<int, int> waveSize = currentSize(WaveformScopeId);
    const std::pair<int, int> histSize = currentSize(HistogramScopeId);
    const std::pair<int, int> vecSize = currentSize(VectorscopeScopeId);
    const auto [wantColumns, wantHeight] = desiredWaveformSize(density, regionWidth);
    const auto [wantHistWidth, wantHistHeight] = desiredHistogramSize(density);
    const int wantVectorscope = desiredVectorscopeSize(density);

    const bool differs = wantColumns != waveSize.first || wantHeight != waveSize.second ||
                         wantVectorscope != vecSize.second || wantHistWidth != histSize.first ||
                         wantHistHeight != histSize.second;
    if (!differs) {
        m_pendingColumns = 0;
    } else if (m_pendingColumns != wantColumns || m_pendingImageHeight != wantHeight ||
               m_pendingVectorscope != wantVectorscope || m_pendingHistWidth != wantHistWidth ||
               m_pendingHistHeight != wantHistHeight) {
        m_pendingColumns = wantColumns;
        m_pendingImageHeight = wantHeight;
        m_pendingVectorscope = wantVectorscope;
        m_pendingHistWidth = wantHistWidth;
        m_pendingHistHeight = wantHistHeight;
        m_detailPendingSince = glfwGetTime();
    } else if (glfwGetTime() - m_detailPendingSince > 0.4) {
        m_analysis.imageSizes[WaveformScopeId] = {wantColumns, wantHeight};
        m_analysis.imageSizes[ParadeScopeId] = {wantColumns, wantHeight};
        m_analysis.imageSizes[VectorscopeScopeId] = {wantVectorscope, wantVectorscope};
        m_analysis.imageSizes[HistogramScopeId] = {wantHistWidth, wantHistHeight};
        m_analysisDirty = true;
    }
}

void App::handleShortcuts(const ModifierState& modifiers)
{
    // Command, Control, and Option chords belong to the system and the window,
    // so any of them silences the plain-letter shortcuts. Shift alone stays
    // meaningful: it stacks.
    const bool systemChord = modifiers.command || modifiers.control || modifiers.option;
    handleCommandChords(modifiers);
    handleControlChords(modifiers);
    handleLetterShortcuts(modifiers, systemChord);
}

void App::handleCommandChords(const ModifierState& modifiers)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (platformHidesWindowOnCommandW() && modifiers.command && !modifiers.control && !modifiers.option &&
        !io.WantTextInput) {
        // Cmd+W dismisses through the system hide - the exact machinery behind
        // Cmd+H, so the Dock click or Cmd+Tab restores every window natively,
        // the border included.
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            hideApplication();
        }
        // Cmd+comma opens settings everywhere on macOS.
        if (ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
            m_showSettings = true;
        }
    }
}

void App::handleControlChords(const ModifierState& modifiers)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (platformMinimizesWindowOnControlW() && modifiers.control && !modifiers.command && !modifiers.option &&
        !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_W, false)) {
        glfwIconifyWindow(m_window);
    }
    if (platformQuitsOnControlQ() && modifiers.control && !modifiers.command && !modifiers.option &&
        !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
    }
}

void App::handleLetterShortcuts(const ModifierState& modifiers, bool systemChord)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || systemChord) {
        return;
    }
    // Each scope's toggle key is resolved by id; a letterless scope has an empty
    // binding, which never matches a press.
    for (const HostScope& scope : m_scopeRegistry.scopes()) {
        if (shortcutPressed(bindingFor(scope.id))) {
            chooseScope(scope.id, modifiers.shift);
        }
    }
    for (const std::string& binding :
         {m_shortcuts.attachWindow, m_shortcuts.drawRegion, m_shortcuts.attachFace, m_shortcuts.pinColor}) {
        if (shortcutPressed(binding)) {
            triggerShortcut(binding, modifiers.shift);
        }
    }
    handleViewShortcuts();
    handlePresetShortcuts(modifiers);
}

// The single map from a shortcut key to its action, shared by the ImGui
// press detection above and the border panel's forwarded keys.
// @return Whether the key matched anything.
bool App::triggerShortcut(const std::string& key, bool shift)
{
    for (const HostScope& scope : m_scopeRegistry.scopes()) {
        if (!bindingFor(scope.id).empty() && bindingFor(scope.id) == key) {
            chooseScope(scope.id, shift);

            return true;
        }
    }
    if (key == m_shortcuts.attachWindow) {
        m_regionPicker.request(RegionPickerMode::AttachWindow);
    } else if (key == m_shortcuts.drawRegion) {
        m_regionPicker.request(RegionPickerMode::DrawGlobal);
    } else if (key == m_shortcuts.attachFace && supportsFaceDetection()) {
        m_regionPicker.request(RegionPickerMode::AttachFace);
    } else if (key == m_shortcuts.pinColor && pinsAvailable()) {
        // One pin tool; each click inside decides between pin-and-close and
        // Shift's pin-and-continue.
        m_regionPicker.request(RegionPickerMode::PinColor);
    } else if (key == m_shortcuts.vectorscopeZoom) {
        m_view.setZoom(m_view.zoom() >= 4 ? 1 : m_view.zoom() * 2);
    } else if (key == m_shortcuts.fullScreen) {
        resetToFullScreen();
    } else {
        return false;
    }

    return true;
}

void App::handlePresetShortcuts(const ModifierState& modifiers)
{
    // Digit N loads preset slot N; Shift+N saves the current layout into it. The
    // guard against text input and system chords is the caller's, shared with
    // the letter shortcuts.
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const auto key = static_cast<ImGuiKey>(ImGuiKey_0 + slot);
        if (!ImGui::IsKeyPressed(key, false)) {
            continue;
        }
        if (modifiers.shift) {
            saveLayoutPreset(slot);
        } else {
            loadLayoutPreset(slot);
        }
    }
}

std::map<std::string, double> App::currentStackWeights() const
{
    // A self-contained snapshot: every scope on screen with its current weight,
    // so a loaded preset reproduces the exact split even for scopes left at the
    // default weight.
    std::map<std::string, double> weights;
    for (const std::string& id : m_view.stack()) {
        weights[id] = m_view.weight(id);
    }

    return weights;
}

void App::saveLayoutPreset(int slot)
{
    m_presetStore.save(slot, capturePreset());
    setStatus("preset " + std::to_string(slot) + " saved");
    m_nextPreferencesSave = glfwGetTime() + 1.0;
}

LayoutPreset App::capturePreset() const
{
    LayoutPreset preset;
    preset.stack = m_view.stackTokens();
    preset.orientation = orientationToInt(m_view.orientation());
    preset.weights = currentStackWeights();
    preset.styles = currentStackStyles();

    return preset;
}

bool App::activePresetDirty() const
{
    return m_presetStore.isDirty(capturePreset());
}

void App::drawPresetPicker()
{
    // A chip like the scope letters, leading the row: the label names the
    // active slot (starred once the live layout drifts; "-" when none), and
    // clicking opens the slot list - the mouse mirror of the digit keys.
    const bool dirty = activePresetDirty();
    char preview[8] = "-";
    if (m_presetStore.activeSlot() != 0) {
        std::snprintf(preview, sizeof(preview), "%d%s", m_presetStore.activeSlot(), dirty ? "*" : "");
    }
    if (scopeToggleButton("##preset-picker", preview, false, "Layout presets - digits load, Shift+digits save")) {
        ImGui::OpenPopup("##preset-popup");
    }
    const ImVec2 chipMin = ImGui::GetItemRectMin();
    const ImVec2 chipMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(chipMin.x, chipMax.y + 2.0f));
    if (ImGui::BeginPopup("##preset-popup")) {
        for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
            const LayoutPreset& preset = m_presetStore.at(slot);
            if (ImGui::Selectable(presetLabel(slot, preset).c_str(), slot == m_presetStore.activeSlot())) {
                if (ImGui::GetIO().KeyShift) {
                    saveLayoutPreset(slot);
                } else {
                    loadLayoutPreset(slot);
                }
            }
        }
        ImGui::TextDisabled("click loads - Shift+click saves");
        ImGui::EndPopup();
    }
}

std::map<std::string, std::map<std::string, double>> App::currentStackStyles() const
{
    std::map<std::string, std::map<std::string, double>> styles;
    for (const std::string& scopeId : m_view.stack()) {
        const HostScope* hostScope = m_scopeRegistry.byId(scopeId);
        if (hostScope == nullptr || hostScope->descriptor == nullptr) {
            continue;
        }
        const std::map<std::string, double>& params = paramsOf(scopeId);
        for (uint32_t index = 0; index < hostScope->descriptor->param_count; ++index) {
            const SsParamInfo& info = hostScope->descriptor->params[index];
            if (info.kind != SS_PARAM_CHOICE) {
                continue;
            }
            const auto current = params.find(info.key);
            styles[scopeId][info.key] = current != params.end() ? current->second : info.default_value;
        }
    }

    return styles;
}

void App::applyPresetStyles(const std::map<std::string, std::map<std::string, double>>& styles)
{
    for (const auto& [scopeId, params] : styles) {
        const HostScope* hostScope = m_scopeRegistry.byId(scopeId);
        if (hostScope == nullptr || hostScope->descriptor == nullptr) {
            continue;
        }
        for (const auto& [key, value] : params) {
            const SsParamInfo* info = findParam(hostScope->descriptor, key);
            if (info == nullptr || info->kind != SS_PARAM_CHOICE) {
                continue;
            }
            m_analysis.scopeParams[scopeId][key] = std::clamp(value, info->min_value, info->max_value);
        }
    }
}

void App::loadLayoutPreset(int slot)
{
    const LayoutPreset& preset = m_presetStore.at(slot);
    if (preset.stack.empty()) {
        setStatus("preset " + std::to_string(slot) + " is empty");

        return;
    }
    m_view.restoreStack(preset.stack);
    m_view.setOrientation(orientationFromInt(preset.orientation));
    m_view.setWeights(preset.weights);
    applyPresetStyles(preset.styles);
    m_presetStore.markLoaded(slot);
    m_analysis.enabledScopes = m_view.enabledScopeIds();
    m_analysisDirty = true;
    setStatus("preset " + std::to_string(slot) + " loaded");
}

void App::handleViewShortcuts()
{
    if (shortcutPressed(m_shortcuts.vectorscopeZoom)) {
        m_view.setZoom(m_view.zoom() >= 4 ? 1 : m_view.zoom() * 2);
    }
    if (shortcutPressed(m_shortcuts.fullScreen)) {
        // Escape peels back one layer at a time: the settings window first, the
        // drawn region only when nothing is stacked above it.
        if (m_showSettings) {
            m_showSettings = false;
        } else {
            resetToFullScreen();
        }
    }
}

// Lazily rasterized textures for the embedded icon set, rebuilt when the
// framebuffer scale changes the requested pixel size.
ImTextureID App::iconTextureId(Icon icon, int sizePixels)
{
    IconTexture& slot = m_iconTextures[static_cast<std::size_t>(icon)];
    if (!slot.texture || slot.sizePixels != sizePixels) {
        ScopeImage image;
        image.width = sizePixels;
        image.height = sizePixels;
        image.rgba = rasterizeIcon(icon, sizePixels);
        slot.texture = m_graphics->createScopeTexture(sizePixels, sizePixels);
        slot.texture->upload(image);
        slot.sizePixels = sizePixels;
    }

    return slot.texture->textureId();
}

std::vector<float> App::stackAspects() const
{
    // A module's own declaration wins; the host table covers the color
    // picker and modules that declare nothing.
    std::vector<float> aspects;
    aspects.reserve(m_view.stack().size());
    for (const std::string& scopeId : m_view.stack()) {
        const HostScope* hostScope = m_scopeRegistry.byId(scopeId);
        const float declared =
            hostScope != nullptr && hostScope->descriptor != nullptr ? hostScope->descriptor->preferred_aspect : 0.0f;
        aspects.push_back(declared > 0.0f ? declared : preferredScopeAspect(scopeId));
    }

    return aspects;
}

void App::handleContextMenu()
{
    // Right-click: the native menu carries the modes and toggles.
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    // Which pane the click landed in decides which options lead the menu.
    const ImVec2 mouse = ImGui::GetMousePos();
    int clickedPane = -1;
    for (std::size_t pane = 0; pane < m_paneRects.size(); ++pane) {
        const ImVec4& rect = m_paneRects[pane];
        if (rect.z <= rect.x || rect.w <= rect.y) {
            continue;
        }
        if (mouse.x >= rect.x && mouse.x < rect.z && mouse.y >= rect.y && mouse.y < rect.w) {
            clickedPane = static_cast<int>(pane);
        }
    }
    std::vector<NativeMenuItem> menu;
    std::vector<ParamMenuAction> paramActions;
    const ContextMenuModel model{
        m_view,        m_scopeRegistry,     m_shortcuts,    m_scopeShortcuts,           m_analysis.scopeParams,
        m_attach,      m_presetStore.all(), m_pins.empty(), m_presetStore.activeSlot(), m_userUiScaleFactor,
        isFullScreen()};
    buildContextMenu(model, clickedPane, menu, paramActions);
    const int chosen = showNativeContextMenu(menu);
    dispatchMenuChoice(chosen, paramActions);
}

const std::map<std::string, double>& App::paramsOf(std::string_view id) const
{
    static const std::map<std::string, double> noParams;
    const auto stored = m_analysis.scopeParams.find(std::string{id});

    return stored != m_analysis.scopeParams.end() ? stored->second : noParams;
}

void App::dispatchMenuChoice(int chosen, const std::vector<ParamMenuAction>& paramActions)
{
    // A dynamic id sets one scope parameter from the side table; the fixed ids
    // drive the host actions.
    if (chosen >= ParamMenuActionBase) {
        const std::size_t index = static_cast<std::size_t>(chosen - ParamMenuActionBase);
        if (index < paramActions.size()) {
            const ParamMenuAction& picked = paramActions[index];
            m_analysis.scopeParams[picked.scopeId][picked.paramKey] = picked.value;
            m_analysisDirty = true;
        }
    }
    dispatchScopeToggleMenu(chosen);
    dispatchRegionMenu(chosen);
    dispatchViewMenu(chosen);
    dispatchLayoutMenu(chosen);
    dispatchUiScaleMenu(chosen);
    m_lastActivity = glfwGetTime();
    m_nextPreferencesSave = glfwGetTime() + 1.0;
}

void App::dispatchScopeToggleMenu(int chosen)
{
    switch (chosen) {
    case MenuShowVectorscope:
        toggleScope(VectorscopeScopeId);
        break;
    case MenuShowWaveform:
        toggleScope(WaveformScopeId);
        break;
    case MenuShowWaveformParade:
        toggleScope(ParadeScopeId);
        break;
    case MenuShowHistogram:
        toggleScope(HistogramScopeId);
        break;
    case MenuShowColorPicker:
        toggleScope(ColorPickerScopeId);
        break;
    default:
        break;
    }
}

void App::dispatchRegionMenu(int chosen)
{
    switch (chosen) {
    case MenuAttachWindow:
        m_regionPicker.request(RegionPickerMode::AttachWindow);
        break;
    case MenuDrawRegion:
        m_regionPicker.request(RegionPickerMode::DrawGlobal);
        break;
    case MenuAttachFace:
        m_regionPicker.request(RegionPickerMode::AttachFace);
        break;
    case MenuFullScreen:
    case MenuDetachAll:
        resetToFullScreen();
        break;
    case MenuDetachWindow:
        detachActiveWindow();
        break;
    case MenuPinColor:
        m_regionPicker.request(RegionPickerMode::PinColor);
        break;
    case MenuClearPinnedMarkers:
        m_pins.clear();
        break;
    default:
        break;
    }
}

// Opens the folder holding the diagnostic log, so "send the log" is a
// click instead of a hunt through the temp directory.
void openDiagLogFolder()
{
    std::string folder = diagLogPath();
    std::replace(folder.begin(), folder.end(), '\\', '/');
    const std::size_t cut = folder.find_last_of('/');
    if (cut == std::string::npos) {
        return;  // a bare file name names no folder to show
    }
    folder.resize(cut == 0 ? 1 : cut);  // a file at the root keeps the root
    const std::string url = (folder.front() == '/' ? "file://" : "file:///") + folder;
    openUrl(url.c_str());
}

void App::dispatchViewMenu(int chosen)
{
    switch (chosen) {
    case MenuZoom1:
        m_view.setZoom(1);
        break;
    case MenuZoom2:
        m_view.setZoom(2);
        break;
    case MenuZoom4:
        m_view.setZoom(4);
        break;
    case MenuToggleGraticule:
        m_view.setGraticule(!m_view.graticule());
        break;
    case MenuToggleCaptureVisibility:
        setCaptureVisibility(!captureVisible());
        break;
    case MenuToggleDiagRecording:
        // The menu records everything; channel selection stays with the
        // SIDESCOPES_DIAG environment for development use.
        diagConfigure(diagRecording() ? DiagConfig{} : DiagConfig{"all", "", DiagFlush::Interval});
        break;
    case MenuShowDiagLog:
        openDiagLogFolder();
        break;
    case MenuResetDiagnostics:
        setCaptureVisibility(false);
        if (diagRecording()) {
            diagConfigure(DiagConfig{});
        }
        break;
    case MenuAbout:
        m_showAbout = true;
        break;
    case MenuOpenSettings:
        m_showSettings = true;
        break;
    case MenuQuit:
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        break;
    default:
        break;
    }
}

void App::dispatchLayoutMenu(int chosen)
{
    // Orientation is a direct set; the preset ranges each map their id back to a
    // slot. Persistence rides dispatchMenuChoice's tail.
    switch (chosen) {
    case MenuLayoutAuto:
        m_view.setOrientation(LayoutOrientation::Automatic);

        return;
    case MenuLayoutVertical:
        m_view.setOrientation(LayoutOrientation::Vertical);

        return;
    case MenuLayoutHorizontal:
        m_view.setOrientation(LayoutOrientation::Horizontal);

        return;
    default:
        break;
    }
    if (chosen > MenuLoadPresetBase && chosen <= MenuLoadPresetBase + LayoutPresetSlots) {
        loadLayoutPreset(chosen - MenuLoadPresetBase);
    } else if (chosen > MenuSavePresetBase && chosen <= MenuSavePresetBase + LayoutPresetSlots) {
        saveLayoutPreset(chosen - MenuSavePresetBase);
    }
}

void App::dispatchUiScaleMenu(int chosen)
{
    const int step = chosen - MenuUiScaleBase;
    if (step < 0 || step >= static_cast<int>(UiScaleSteps.size())) {
        return;
    }
    m_userUiScaleFactor = UiScaleSteps[static_cast<std::size_t>(step)];
    refreshUiScale();
}

void App::commitAnalysisChanges()
{
    if (m_analysisDirty) {
        m_worker.updateSettings(m_analysis);
        configureProjectionInstances();
        syncRegionBorder();
        m_analysisDirty = false;
        m_lastActivity = glfwGetTime();
        m_nextPreferencesSave = glfwGetTime() + 1.0;
    }
    if (m_nextPreferencesSave > 0.0 && glfwGetTime() > m_nextPreferencesSave) {
        persistPreferences();
        m_nextPreferencesSave = -1.0;
    }
}

}  // namespace sidescopes
