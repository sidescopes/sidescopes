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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "app/about_window.h"
#include "app/adaptive_detail.h"
#include "app/app_startup.h"
#include "app/capture_controller.h"
#include "app/color_readout.h"
#include "app/context_menu.h"
#include "app/frame_pacing.h"
#include "app/frame_timer.h"
#include "app/interface_style.h"
#include "app/overlay_render.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/region_coordinator.h"
#include "app/region_geometry.h"
#include "app/row_layout.h"
#include "app/scope_layout.h"
#include "app/scope_pane_renderer.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/settings_window.h"
#include "app/ui_scale.h"
#include "app/ui_scaling.h"
#include "app/version.h"
#include "app/window_suggestions.h"
#include "core/analysis_worker.h"
#include "core/color_lab.h"
#include "core/diagnostics.h"
#include "core/frame_mailbox.h"
#include "core/preferences.h"
#include "core/region_suggestions.h"
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

// A key the resolver names resolves to the ImGui key it fires on: the letters
// and Escape a binding may hold, plus the preset digits and the comma of the
// settings chord. Anything else never matches a press.
ImGuiKey keyFor(std::string_view name)
{
    if (name == "Escape") {
        return ImGuiKey_Escape;
    }
    if (name == "Comma") {
        return ImGuiKey_Comma;
    }
    if (name.size() != 1) {
        return ImGuiKey_None;
    }
    if (name[0] >= 'A' && name[0] <= 'Z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'A'));
    }
    if (name[0] >= '1' && name[0] <= '9') {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (name[0] - '0'));
    }

    return ImGuiKey_None;
}

// The resolver's key probe, bound to the toolkit here and nowhere else.
bool shortcutPressed(std::string_view name)
{
    const ImGuiKey key = keyFor(name);

    return key != ImGuiKey_None && ImGui::IsKeyPressed(key, false);
}

}  // namespace

namespace sidescopes {
namespace {

// Waits until @p due, then handles everything that arrived in one go.
//
// glfwWaitEventsTimeout is a timeout, not a floor: it returns the moment an
// event lands, so a stream of them - a pointer crossing a photograph delivers
// one per movement - ran the loop at the event rate instead of the frame rate.
// Measured at sixty-five frames a second against a sixty-frame cap, for scope
// images that arrive thirty times a second at best.
//
// Sleeping through the period rather than waking on each event costs nothing in
// responsiveness - the events belong to the same frame either way - and saves
// the wake itself, which is most of what a wandering pointer costs: measured at
// sixty wakes a second against one.
void waitOutFramePeriod(double due)
{
    const double left = due - glfwGetTime();
    if (left > 0.0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(left));
    }
    glfwPollEvents();
}

}  // namespace

App::App()
    : m_uiScale(computeUiScale, applyInterfaceScale),
      m_worker(m_mailbox),
      m_capture(createScreenCaptureSource()),
      m_captureController(*m_capture, m_mailbox),
      m_faceLock(m_attach, m_worker, m_captureController),
      m_regionPicker(m_captureController, m_worker, *m_capture),
      m_regions(m_attach, m_captureController, m_regionPicker, m_faceLock, m_analysis.region),
      m_scopeRegistry(builtinModules()),
      m_view(m_scopeRegistry),
      m_shortcuts(m_scopeRegistry),
      m_cursor(m_captureController, m_worker),
      m_presets(m_view, m_scopeRegistry, m_analysis),
      m_detail(m_view, m_analysis)
{
}

bool App::init()
{
    diagInit();
    if (!glfwInit()) {
        return false;
    }

    const Preferences startup = loadPreferences(preferencesFilePath());
    m_versionInfo = describeVersion(SIDESCOPES_VERSION, SIDESCOPES_GIT_DESCRIBE);

    MainWindow mainWindow = createMainWindow(startup, m_versionInfo, m_callbackState);
    if (!mainWindow.window) {
        return false;
    }
    m_window = mainWindow.window;
    m_graphics = std::move(mainWindow.graphics);
    setupImGui();
    if (!m_graphics->init(m_window)) {
        ImGui::DestroyContext();
        glfwDestroyWindow(m_window);
        glfwTerminate();

        return false;
    }
    m_frameTimer = std::make_unique<FrameTimer>(*m_graphics);

    setupCapture();
    seedAnalysis(m_analysis, startup);
    setupView(startup);
    m_shortcuts.restore(startup.shortcuts, startup.scopeShortcuts);
    const ScopePaneContext paneContext{*m_graphics,         m_view,         m_scopeRegistry, m_analysis, m_output,
                                       m_captureController, m_regionPicker, m_pins,          m_shortcuts};
    m_panes = std::make_unique<ScopePaneRenderer>(paneContext, createProjectionInstances(m_scopeRegistry),
                                                  createScopeTextures(m_scopeRegistry));

    m_worker.setOutputCallback([] { glfwPostEmptyEvent(); });
    m_worker.start();
    warmFaceDetection();

    observeSystemWake([this] {
        m_sessionAsleep.store(false);
        m_captureController.markStale();
    });
    observeSystemSleep([this] { m_sessionAsleep.store(true); });
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
    m_regions.syncBorder(borderState());

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
    m_uiScale.refresh(m_window);
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
    m_view.stack().restore(startup.scopeStack);
    m_view.setGraticule(startup.showGraticule);
    m_view.setZoom(startup.vectorscopeZoom);
    m_view.layout().setOrientation(orientationFromInt(startup.layoutOrientation));
    m_view.layout().setWeights(startup.layoutWeights);
    m_presets.restore(startup.layoutPresets, startup.layoutActiveSlot);
    // The stored factor is cleaned to an offered step here, at the app boundary,
    // so core preferences never depend on the app's scaling policy. setupImGui
    // already applied the OS scale at the 1.0 default; fold the preference in now,
    // before the first frame.
    m_uiScale.restore(startup.uiScaleFactor, m_window);
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
    m_view.traces().setIntensity(VectorscopeScopeId,
                                 intensityFromTraceGain(static_cast<float>(scopeParam(VectorscopeScopeId, "gain", 3.0)),
                                                        VectorscopeIntensityShift));
    m_view.traces().setIntensity(WaveformScopeId,
                                 intensityFromTraceGain(static_cast<float>(scopeParam(WaveformScopeId, "gain", 0.05))));
    m_view.traces().setSmoothing(VectorscopeScopeId, startupSmoothing(VectorscopeScopeId, 75.0));
    m_view.traces().setSmoothing(WaveformScopeId, startupSmoothing(WaveformScopeId, 100.0));
    m_analysis.enabledScopes = m_view.stack().enabledScopeIds();
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

bool App::pinsAvailable() const
{
    // Pins mark any scope that declares itself a pin target (plus the host's
    // own color picker); without one on screen, the tool's button, menu
    // entries, and shortcuts all stand down together.
    for (const std::string& scopeId : m_view.stack().ids()) {
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

void App::refreshActivatedScope(std::string_view id)
{
    // A scope draws the same frame it turns on, but the worker only computes
    // what is enabled, so a newly shown scope's image is stale. Turning it on
    // pushes the settings immediately and waits briefly for the recompute; on
    // timeout the stale image stands in until the recompute lands a frame later.
    if (!m_panes->hasTexture(id) || !m_analysis.region) {
        // The color picker asks nothing of the worker, and neither does a
        // scope with no region to read: waiting for an image that cannot
        // arrive would stall every toggle for the whole timeout.
        return;
    }
    const uint64_t staleSequence = m_panes->imageFor(id).sequence;
    m_worker.updateSettings(m_analysis);
    const double deadline = glfwGetTime() + 0.08;
    while (glfwGetTime() < deadline) {
        if (m_worker.fetchOutput(m_outputVersion, m_output) && m_panes->imageFor(id).sequence != staleSequence &&
            m_panes->imageFor(id).width > 0) {
            m_panes->uploadVisibleScopes();

            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    m_panes->uploadVisibleScopes();  // timeout: a stale image beats none
}

void App::toggleScope(std::string_view id)
{
    const bool activated = m_view.stack().toggle(id);
    m_analysis.enabledScopes = m_view.stack().enabledScopeIds();
    if (activated) {
        refreshActivatedScope(id);
    }
    m_analysisDirty = true;
}

void App::chooseScope(std::string_view id, bool stack)
{
    const bool activated = m_view.stack().choose(id, stack);
    m_analysis.enabledScopes = m_view.stack().enabledScopeIds();
    if (activated) {
        refreshActivatedScope(id);
    }
    m_analysisDirty = true;
}

RegionBorderState App::borderState() const
{
    return RegionBorderState{m_attachActiveLabel, m_activeWindowIdentity, m_attachedWindowMoving,
                             glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) != 0, glfwGetTime()};
}

// Applies a region decision to host state. The coordinator owns the global
// region and the border; what lands here is what only the host can carry out -
// the settings the worker reads, the active window the motion watch follows,
// and the clock the idle wait measures against.
void App::applyRegionOutcome(const RegionOutcome& outcome)
{
    if (outcome.detachedAll) {
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
    if (outcome.regionChanged) {
        m_analysis.region = outcome.region;
        m_analysisDirty = true;
        if (!m_analysis.region) {
            // Nothing is being read, so the last region's traces are dropped
            // rather than left standing: the panes go honestly empty, and the
            // next region draws onto a clean instrument instead of a ghost.
            m_panes->releaseTraces();
        }
    }
    if (outcome.activity) {
        m_lastActivity = glfwGetTime();
    }
}

void App::persistPreferences()
{
    Preferences preferences;
    // The worker's parameter map is the persisted state directly; only the
    // host-owned smoothing control is folded back in. The parade is dropped: it
    // mirrors the waveform and re-seeds on load.
    preferences.scopeParams = m_analysis.scopeParams;
    preferences.scopeParams.erase(ParadeScopeId);
    preferences.scopeParams[VectorscopeScopeId]["smoothing_ms"] = m_view.traces().smoothing(VectorscopeScopeId);
    preferences.scopeParams[WaveformScopeId]["smoothing_ms"] = m_view.traces().smoothing(WaveformScopeId);
    preferences.scopeStack = m_view.stack().tokens();
    preferences.showGraticule = m_view.graticule();
    preferences.vectorscopeZoom = m_view.zoom();
    preferences.layoutOrientation = orientationToInt(m_view.layout().orientation());
    preferences.layoutWeights = m_view.layout().weightsSnapshot();
    preferences.layoutPresets = m_presets.all();
    preferences.layoutActiveSlot = m_presets.activeSlot();
    preferences.uiScaleFactor = m_uiScale.userFactor();
    preferences.shortcuts = m_shortcuts.bindings();
    preferences.scopeShortcuts = m_shortcuts.scopeOverrides();
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
    // would; the phase methods below fill them in. The cursor colors need no
    // clearing here: the sampler writes all three every frame that draws.
    m_regionPicker.clearRequest();

    pumpEvents();
    m_frameTimer->markFrameBodyStart();
    drainAsyncSignals();
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    const bool nothingToDrawInto = framebufferWidth == 0 || framebufferHeight == 0;
    // Capture is a service that dies (lock screen, display sleep); restarting
    // it is our job.
    servicePipelineVisibility(nothingToDrawInto, glfwGetTime());
    m_captureController.service(glfwGetTime());
    // Attached regions: observe the attached windows and route the analysis by
    // the focused window. The border reconciles here every frame in both
    // regimes, so no missed edge can strand it on screen.
    followAttachedWindow();
    m_regions.syncBorder(borderState());
    followWindowDisplay();
    syncUiScaleToMonitor();

    if (nothingToDrawInto) {
        return;
    }
    if (!m_graphics->beginFrame(framebufferWidth, framebufferHeight)) {
        return;
    }

    if (m_worker.fetchOutput(m_outputVersion, m_output)) {
        m_panes->uploadVisibleScopes();
        SS_DIAG(Perf, "pass analysis_ms=%.1f", m_output.accumulateMilliseconds);
        m_lastActivity = glfwGetTime();
    }
    m_frameSize = m_worker.latestFrameSize();
    publishSelfWindowMask();
    sampleCursorColor();
    updateAdaptiveDetail(framebufferWidth);

    drawFrameUi();

    ImGui::Render();
    m_frameTimer->presentFrame();

    // Second follow step, right after the vsync wait: the pre-frame geometry
    // is a frame stale by now, and a border moved from it would trail a
    // fast-dragged window visibly.
    followAttachedWindow();

    // The blocking overlay runs after the frame is submitted; capture and
    // analysis keep flowing underneath.
    applyRegionPickOutcome(m_regionPicker.openIfRequested(m_analysis.region.has_value()));
    applyBorderEditOutcome(m_regions.pollBorderEdit(m_activeWindowIdentity));
    applyRegionPickOutcome(m_regionPicker.poll(m_frameSize, m_cursor.screenSampleColor()));
    commitAnalysisChanges();
}

void App::servicePipelineVisibility(bool framebufferEmpty, double now)
{
    const VisibilityInputs inputs{
        m_sessionAsleep.load(),
        applicationHidden(),
        glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) != 0,
        glfwGetWindowAttrib(m_window, GLFW_VISIBLE) != 0,
        framebufferEmpty,
        m_regionPicker.active() || m_regionPicker.scansRunning() || m_faceLock.probeRunning()};

    switch (m_visibility.update(inputs, m_captureController.suspended(), now)) {
    case PipelineAction::Suspend:
        SS_DIAG(Perf, "pipeline suspended - out of sight");
        m_captureController.suspend();
        break;
    case PipelineAction::Resume:
        SS_DIAG(Perf, "pipeline resumed");
        m_captureController.resume();
        break;
    case PipelineAction::Keep:
        break;
    }

    serviceCaptureCrop(inputs.needsFrames, now);
}

// Asks the capture for only the pixels the region needs. The compositor renders,
// scales and delivers whatever the stream is configured for, so a region a
// fraction of the display was still costing a whole display's worth of that work
// every frame, plus the copy into the mailbox.
void App::serviceCaptureCrop(bool otherReadersActive, double now)
{
    if (!m_frameSize) {
        return;
    }
    // No region narrows nothing: the colour under the pointer is read from the
    // stream wherever it lands, and a narrowed stream would leave that reading
    // to the far slower whole-screen sample for the whole of an empty session.
    const IntRect regionPixels =
        m_analysis.region ? m_analysis.region->toPixels(m_frameSize->displayWidth, m_frameSize->displayHeight)
                          : IntRect{0, 0, m_frameSize->displayWidth, m_frameSize->displayHeight};
    const std::optional<IntRect> crop =
        m_cropTracker.decide(regionPixels, m_frameSize->displayWidth, m_frameSize->displayHeight, otherReadersActive,
                             m_faceLock.locked(), now);
    if (crop != m_appliedCrop) {
        SS_DIAG(Perf, "capture narrowed to %dx%d at %d,%d", crop ? crop->width : m_frameSize->displayWidth,
                crop ? crop->height : m_frameSize->displayHeight, crop ? crop->x : 0, crop ? crop->y : 0);
        m_appliedCrop = crop;
    }
    m_captureController.narrowTo(crop);
}

void App::pumpEvents()
{
    const double now = glfwGetTime();
    const FrameWaitDecision wait = frameWaitFor(FramePacingInputs{
        now, m_lastActivity, m_lastReadoutActivity, m_lastFrameStart, m_attach.attached(), m_regionPicker.active()});
    switch (wait.kind) {
    case FrameWait::WatchAttachedWindow:
        idleWaitWatchingAttachedWindow();
        break;
    case FrameWait::Idle:
        // Woken by any event, which is what keeps the application feeling
        // instant while nothing is happening.
        glfwWaitEventsTimeout(IdleWaitSeconds);
        break;
    case FrameWait::None:
        break;  // the frame period below is the whole wait
    }
    // Whatever ended that wait, the frame period is a floor: a wait that ends
    // on the first event redraws at the event rate otherwise.
    waitOutFramePeriod(now + wait.redrawFloorSeconds);
    m_lastFrameStart = glfwGetTime();
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
        m_regions.syncBorder(borderState());
        m_lastActivity = glfwGetTime();
    }
    if (m_orphanEscape.exchange(false)) {
        applyRegionOutcome(m_regions.clearRegion());
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
            applyShortcutAction(m_shortcuts.resolveNamed(press.key, press.shift, shortcutContext()));
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
        !m_analysis.region && !m_attach.attached()) {
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
    // factor rides along through the controller's refresh.
    if (m_uiScale.refresh(m_window)) {
        m_lastActivity = glfwGetTime();
    }
}

void App::publishSelfWindowMask()
{
    // Publish our own window rectangle (display pixels, generous chrome margins)
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
    // Display pixels per point, not FRAME pixels per point: the mask is stated
    // in display pixels so it survives a capture narrowed to part of one.
    const double scaleX = m_frameSize->displayWidth / geometry->widthPoints;
    const double scaleY = m_frameSize->displayHeight / geometry->heightPoints;
    // The chrome margins are 100%-scale units like the rest of the interface,
    // so they grow with the monitor's scale.
    const IntRect selfWindow{static_cast<int>((windowX - geometry->originX - 8.0f * m_uiScale.scale()) * scaleX),
                             static_cast<int>((windowY - geometry->originY - 42.0f * m_uiScale.scale()) * scaleY),
                             static_cast<int>((static_cast<float>(windowW) + 16.0f * m_uiScale.scale()) * scaleX),
                             static_cast<int>((static_cast<float>(windowH) + 58.0f * m_uiScale.scale()) * scaleY)};
    if (selfWindow.x != m_analysis.maskedWindow.x || selfWindow.y != m_analysis.maskedWindow.y ||
        selfWindow.width != m_analysis.maskedWindow.width || selfWindow.height != m_analysis.maskedWindow.height) {
        m_analysis.maskedWindow = selfWindow;
        m_analysisDirty = true;
    }
}

// Applies a cursor sample to host state: the smoothed colors flow on to this
// frame's drawing, and a marker that moved counts as interaction.
void App::sampleCursorColor()
{
    const CursorSmoothing smoothing{m_view.traces().smoothing(VectorscopeScopeId),
                                    m_view.traces().smoothing(WaveformScopeId)};
    const CursorSample sample =
        m_cursor.update(m_frameSize, m_analysis.region, smoothing, glfwGetTime(), ImGui::GetIO().DeltaTime);
    m_vectorscopeColor = sample.vectorscopeColor;
    m_waveformColor = sample.waveformColor;
    m_readoutColor = sample.readoutColor;
    if (sample.changed) {
        m_lastActivity = glfwGetTime();
    }
    if (sample.readoutChanged) {
        m_lastReadoutActivity = glfwGetTime();
    }
}

void App::updateAdaptiveDetail(int framebufferWidth)
{
    const auto paneSize = [this](std::string_view id) {
        const ImVec2 points = m_panes->paneSizePoints(id);

        return PaneSize{points.x, points.y};
    };
    int windowW = 0;
    int windowH = 0;
    glfwGetWindowSize(m_window, &windowW, &windowH);
    const float density = windowW > 0 ? static_cast<float>(framebufferWidth) / static_cast<float>(windowW) : 1.0f;
    const ScopePaneSizes panes{paneSize(WaveformScopeId), paneSize(ParadeScopeId), paneSize(HistogramScopeId),
                               paneSize(VectorscopeScopeId), paneSize(NeutralScopeId)};
    const std::optional<DetailSizes> sizes = m_detail.update(panes, density, m_frameSize, glfwGetTime());
    if (!sizes) {
        return;
    }
    m_analysis.imageSizes[WaveformScopeId] = sizes->waveform;
    m_analysis.imageSizes[ParadeScopeId] = sizes->waveform;
    m_analysis.imageSizes[VectorscopeScopeId] = {sizes->vectorscope, sizes->vectorscope};
    m_analysis.imageSizes[HistogramScopeId] = sizes->histogram;
    m_analysis.imageSizes[NeutralScopeId] = {sizes->neutral, sizes->neutral};
    m_analysisDirty = true;
}

void App::drawFrameUi()
{
    ImGui::NewFrame();
    beginHostWindow();

    // The stacking modifier reads the OS's live key state, not the event-tracked
    // one: a Shift key-up swallowed by a system overlay leaves the cache stuck
    // exactly when the user next switches a scope.
    const ModifierState modifiers = currentModifiers();
    applyPresetOutcome(m_presets.drawPicker());
    ImGui::SameLine(0.0f, 8.0f);
    applyPaneRenderOutcome(m_panes->drawScopeToggles(modifiers.shift));
    for (const ShortcutAction& action : m_shortcuts.resolvePressed(shortcutContext(), modifiers, shortcutPressed)) {
        applyShortcutAction(action);
    }
    const PaneRenderInput input{
        m_uiScale.scale(), m_analysis.region.has_value(), pinsAvailable(), m_vectorscopeColor, m_waveformColor,
        m_readoutColor,    m_callbackState.monospaceFont};
    applyPaneRenderOutcome(m_panes->drawRegionToolIcons(input));
    applyPaneRenderOutcome(m_panes->drawScopePanes(input));
    m_panes->drawStatusBar(input);
    handleContextMenu();

    ImGui::End();
    ImGui::PopStyleVar();
    applyPendingUiScale();

    const SettingsContext settingsCtx{m_showSettings,  m_view,   m_analysis,    m_analysisDirty,
                                      m_scopeRegistry, m_output, m_versionInfo, m_captureController.status()};
    drawSettingsWindow(settingsCtx);
    m_about.draw(m_versionInfo);

    if (ImGui::IsAnyItemActive()) {
        m_lastActivity = glfwGetTime();
        m_nextPreferencesSave = glfwGetTime() + 1.0;
    }
}

void App::beginHostWindow()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::Begin("##host", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
}

// Applies a pane-render outcome to host state. The renderer drives the view,
// the picker, and the pin board itself; what lands here is what only the host
// can carry out - bringing a scope on screen, dropping every region, and the
// clocks the whole shell shares.
void App::applyPaneRenderOutcome(const PaneRenderOutcome& outcome)
{
    if (outcome.chosenScope) {
        chooseScope(outcome.chosenScope->id, outcome.chosenScope->stack);
    }
    if (outcome.reorderedStack) {
        // A view-only rearrangement: the scope set is unchanged, so the panes
        // reflow from the new stack order with no worker recompute.
        m_view.stack().reorder(*outcome.reorderedStack);
    }
    if (outcome.clearRegion) {
        applyRegionOutcome(m_regions.clearRegion());
    }
    if (outcome.analysisDirty) {
        m_analysisDirty = true;
    }
    if (outcome.activity) {
        m_lastActivity = glfwGetTime();
    }
    if (outcome.preferencesSaveDue) {
        m_nextPreferencesSave = glfwGetTime() + 1.0;
    }
}

ShortcutContext App::shortcutContext() const
{
    ShortcutContext context;
    context.wantsTextInput = ImGui::GetIO().WantTextInput;
    context.faceDetectionSupported = supportsFaceDetection();
    context.pinsAvailable = pinsAvailable();
    context.settingsOpen = m_showSettings;
    context.vectorscopeZoom = m_view.zoom();
    context.hidesWindowOnCommandW = platformHidesWindowOnCommandW();
    context.minimizesWindowOnControlW = platformMinimizesWindowOnControlW();
    context.quitsOnControlQ = platformQuitsOnControlQ();

    return context;
}

// Carries out what the resolver decided. Which scope, which tool, which zoom
// level, which layer to peel - all of that is settled by the time it arrives;
// what is left is the shell state only the host can reach.
void App::applyShortcutAction(const ShortcutAction& action)
{
    switch (action.kind) {
    case ShortcutAction::Kind::ChooseScope:
        chooseScope(action.scopeId, action.stack);
        break;
    case ShortcutAction::Kind::RequestPick:
        m_regionPicker.request(action.pickMode);
        break;
    case ShortcutAction::Kind::SetZoom:
        m_view.setZoom(action.zoomLevel);
        break;
    case ShortcutAction::Kind::CloseSettings:
        m_showSettings = false;
        break;
    case ShortcutAction::Kind::ClearRegion:
        applyRegionOutcome(m_regions.clearRegion());
        break;
    case ShortcutAction::Kind::LoadPreset:
        applyPresetOutcome(m_presets.load(action.presetSlot));
        break;
    case ShortcutAction::Kind::SavePreset:
        applyPresetOutcome(m_presets.save(action.presetSlot));
        break;
    case ShortcutAction::Kind::HideApplication:
        hideApplication();
        break;
    case ShortcutAction::Kind::MinimizeWindow:
        glfwIconifyWindow(m_window);
        break;
    case ShortcutAction::Kind::QuitWindow:
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        break;
    case ShortcutAction::Kind::OpenSettings:
        m_showSettings = true;
        break;
    case ShortcutAction::Kind::None:
        break;
    }
}

// Applies what a preset action decided. The controller has already moved the
// view and the stored slots; the strip, the worker, and the preferences file
// are the host's to bring along.
void App::applyPresetOutcome(const LayoutPresetOutcome& outcome)
{
    if (!outcome.status.empty()) {
        setStatus(outcome.status);
    }
    if (outcome.analysisDirty) {
        m_analysisDirty = true;
    }
    if (outcome.preferencesSaveDue) {
        m_nextPreferencesSave = glfwGetTime() + 1.0;
    }
}

void App::setStatus(std::string message)
{
    m_panes->setStatus(std::move(message));
    m_lastActivity = glfwGetTime();
}

void App::handleContextMenu()
{
    // Right-click: the native menu carries the modes and toggles.
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    const int clickedPane = m_panes->paneAt(ImGui::GetMousePos());
    std::vector<NativeMenuItem> menu;
    std::vector<ParamMenuAction> paramActions;
    const ContextMenuModel model{m_view,
                                 m_scopeRegistry,
                                 m_shortcuts,
                                 m_analysis.scopeParams,
                                 m_attach,
                                 m_presets.all(),
                                 m_pins.empty(),
                                 m_presets.activeSlot(),
                                 m_uiScale.userFactor(),
                                 m_analysis.region.has_value()};
    buildContextMenu(model, clickedPane, menu, paramActions);
    const int chosen = showNativeContextMenu(menu);
    dispatchMenuChoice(chosen, paramActions);
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
    // The scope-toggle ids carry the scope's registry index, so this resolves
    // any registered scope without naming one.
    const int index = chosen - MenuShowScopeBase;
    const std::vector<HostScope>& scopes = m_scopeRegistry.scopes();
    if (index >= 0 && index < static_cast<int>(scopes.size())) {
        toggleScope(scopes[static_cast<std::size_t>(index)].id);
    }
}

void App::dispatchRegionMenu(int chosen)
{
    // The region tools are the keys' actions under a different hand, so they
    // travel the same road; Clear Region is the menu's own, without the key's
    // peel.
    switch (chosen) {
    case MenuAttachWindow:
        applyShortcutAction(ShortcutAction::pick(RegionPickerMode::AttachWindow));
        break;
    case MenuDrawRegion:
        applyShortcutAction(ShortcutAction::pick(RegionPickerMode::DrawGlobal));
        break;
    case MenuAttachFace:
        applyShortcutAction(ShortcutAction::pick(RegionPickerMode::AttachFace));
        break;
    case MenuClearRegion:
    case MenuDetachAll:
        applyRegionOutcome(m_regions.clearRegion());
        break;
    case MenuDetachWindow:
        detachActiveWindow();
        break;
    case MenuPinColor:
        applyShortcutAction(ShortcutAction::pick(RegionPickerMode::PinColor));
        break;
    case MenuClearPinnedMarkers:
        m_pins.clear();
        break;
    default:
        break;
    }
}

namespace {

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

}  // namespace

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
        m_about.open();
        break;
    case MenuOpenSettings:
        applyShortcutAction(ShortcutAction::plain(ShortcutAction::Kind::OpenSettings));
        break;
    case MenuQuit:
        applyShortcutAction(ShortcutAction::plain(ShortcutAction::Kind::QuitWindow));
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
        m_view.layout().setOrientation(LayoutOrientation::Automatic);

        return;
    case MenuLayoutVertical:
        m_view.layout().setOrientation(LayoutOrientation::Vertical);

        return;
    case MenuLayoutHorizontal:
        m_view.layout().setOrientation(LayoutOrientation::Horizontal);

        return;
    default:
        break;
    }
    if (chosen > MenuLoadPresetBase && chosen <= MenuLoadPresetBase + LayoutPresetSlots) {
        applyShortcutAction(ShortcutAction::preset(chosen - MenuLoadPresetBase, false));
    } else if (chosen > MenuSavePresetBase && chosen <= MenuSavePresetBase + LayoutPresetSlots) {
        applyShortcutAction(ShortcutAction::preset(chosen - MenuSavePresetBase, true));
    }
}

void App::dispatchUiScaleMenu(int chosen)
{
    // Only record the step here - the native menu runs inside the host window's
    // WindowPadding push, and selectStep rebuilds the whole style. applyPending-
    // UiScale runs it once that push is popped.
    const int step = chosen - MenuUiScaleBase;
    if (step >= 0 && step < static_cast<int>(UiScaleSteps.size())) {
        m_pendingUiScaleStep = step;
    }
}

void App::applyPendingUiScale()
{
    if (m_pendingUiScaleStep < 0) {
        return;
    }
    m_uiScale.selectStep(m_pendingUiScaleStep, m_window);
    m_pendingUiScaleStep = -1;
}

void App::commitAnalysisChanges()
{
    const RegionMotion motion = trackRegionMotion(glfwGetTime());
    if (m_analysisDirty) {
        // Coarse only on the way out: the settings themselves stay the truth,
        // so the detail policy and the projections keep reading what the region
        // will be analysed at the moment it stops moving.
        m_worker.updateSettings(motion == RegionMotion::Dragged ? coarsenedForDrag(m_analysis) : m_analysis);
        m_panes->configureProjections();
        m_regions.syncBorder(borderState());
        m_analysisDirty = false;
        m_lastActivity = glfwGetTime();
        m_nextPreferencesSave = glfwGetTime() + 1.0;
    }
    if (m_nextPreferencesSave > 0.0 && glfwGetTime() > m_nextPreferencesSave) {
        persistPreferences();
        m_nextPreferencesSave = -1.0;
    }
}

// What is moving the region decides what analysis does about it, and the two
// causes want opposite things. The user dragging the region - a border by its
// band, a rectangle still being drawn - is scanning a picture and wants the
// scopes live under their hand, so the pass goes on at a coarser image. An
// attached window carrying it across the screen is a desktop being tidied and
// is nobody reading anything, so analysis is held outright and the last images
// stand until it lands.
RegionMotion App::trackRegionMotion(double now)
{
    bool regionChanged = false;
    if (m_analysisDirty && m_analysis.region != m_lastSentRegion) {
        m_lastSentRegion = m_analysis.region;
        regionChanged = true;
    }

    // A live picker means the user's hand is on the region, whatever a window
    // was doing a moment ago - and the flag it would otherwise carry cannot
    // clear while the picker is up, because the follow step that clears it is
    // suppressed for the picker's whole duration.
    const bool carried = m_attachedWindowMoving && !m_regionPicker.active();
    const RegionMotionStep step = m_motion.update({regionChanged, carried, now});
    if (step.changed) {
        m_worker.hold(step.motion == RegionMotion::Carried);
        m_analysisDirty = true;
    }

    return step.motion;
}

}  // namespace sidescopes
