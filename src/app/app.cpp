// The SideScopes application shell, shared by every platform: a compact,
// always-on-top window stacking the enabled scopes. All analysis lives in
// the core library on its own thread; this file owns the interaction
// model (gestures, native menu, region selection) and preferences, while
// rendering and window chrome live behind the graphics seam.

#include "app/app.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

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
#include "app/capture_supervisor.h"
#include "app/color_readout.h"
#include "app/context_menu.h"
#include "app/frame_pacing.h"
#include "app/frame_timer.h"
#include "app/interface_style.h"
#include "app/overlay_render.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/preferences_binding.h"
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
#include "app/window_place.h"
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
#include "platform/frame_pool.h"
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
      m_presetPicker(m_presets),
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
    m_callbackState.monospaceFont = startImGui(m_window);
    m_uiScale.refresh(m_window);
    if (!m_graphics->init(m_window)) {
        stopRendering(m_window, nullptr);

        return false;
    }
    m_frameTimer = std::make_unique<FrameTimer>(*m_graphics);

    // Before the stream is created, so a saved level is the rate it is created
    // at rather than a restart on the way up.
    applyQuality(qualityFromToken(startup.quality));
    setupCapture();
    seedImageSizes(m_analysis);
    restorePreferences(startup, m_view, m_pins, m_shortcuts, m_analysis);
    m_presets.restore(startup.layoutPresets, startup.layoutActiveSlot);
    // The stored factor - or, for a file that names none, the one this display's
    // density recommends - is cleaned to an offered step here, at the app
    // boundary, so core preferences never depend on the app's scaling policy.
    // setupImGui already applied the OS scale at the 1.0 default; fold the
    // preference in now, before the first frame.
    m_uiScale.restore(startupUiScaleFactor(startup, m_window), m_window);
    const ScopePaneContext paneContext{*m_graphics,         m_view,         m_scopeRegistry, m_analysis, m_output,
                                       m_captureController, m_regionPicker, m_pins,          m_shortcuts};
    m_panes = std::make_unique<ScopePaneRenderer>(paneContext, createProjectionInstances(m_scopeRegistry),
                                                  createScopeTextures(m_scopeRegistry));

    m_worker.setOutputCallback([this] { noteWorkerOutput(); });
    m_worker.start();
    warmFaceDetection();

    observeSystemEvents();
    rememberApplicationWindow(m_graphics->nativeWindowHandle());
    m_ownPid = ownApplicationPid();

    m_clocks.noteActivity(glfwGetTime());
    m_regions.syncBorder(borderState());

    return true;
}

void App::observeSystemEvents()
{
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
}

// A pass has been published. Runs on the worker's thread, so it does two
// things and no more: marks the frame loop's picture out of date, and ends
// whatever wait the loop is in.
void App::noteWorkerOutput()
{
    m_clocks.noteOutputPublished();
    glfwPostEmptyEvent();
}

void App::run()
{
    while (!glfwWindowShouldClose(m_window)) {
        runInFramePool([](void* self) { static_cast<App*>(self)->runFrame(); }, this);
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
    stopRendering(m_window, m_graphics.get());
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

WindowPlacement App::windowPlacement() const
{
    WindowPlacement placement;
    glfwGetWindowPos(m_window, &placement.x, &placement.y);
    glfwGetWindowSize(m_window, &placement.width, &placement.height);

    return placement;
}

std::optional<uint32_t> App::displayOfWindow() const
{
    return displayUnderWindow(windowPlacement());
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
            m_panes->uploadVisibleScopes(/*traceLive=*/true);

            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    m_panes->uploadVisibleScopes(/*traceLive=*/true);  // timeout: a stale image beats none
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
        m_clocks.noteActivity(glfwGetTime());
    }
}

void App::persistPreferences()
{
    // Everything the binding does not restore is the shell's own, and this is
    // the whole of it.
    Preferences preferences = capturePreferences(m_view, m_pins, m_shortcuts, m_analysis);
    preferences.layoutPresets = m_presets.all();
    preferences.layoutActiveSlot = m_presets.activeSlot();
    preferences.uiScaleFactor = m_uiScale.userFactor();
    preferences.quality = qualityToken(m_quality);
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
    serviceCapture(nothingToDrawInto, glfwGetTime());
    m_captureController.service(glfwGetTime());
    // Attached regions: observe the attached windows and route the analysis by
    // the focused window. The border reconciles here every frame in both
    // regimes, so no missed edge can strand it on screen.
    followAttachedWindow();
    m_regions.syncBorder(borderState());
    followWindowDisplay();
    syncUiScaleToMonitor();
    notePointerMovement();

    if (nothingToDrawInto) {
        return;
    }
    // The redraw decision is taken before any of the frame is built: everything
    // it rests on is either a clock the loop already keeps or a cheap read, and
    // the expensive part of a frame is the frame.
    const std::string captureStatus = m_captureController.status();
    const RedrawSignals signals{m_callbackState.lastInputEvent,
                                m_panes->redrawDueSeconds(),
                                ImGui::GetIO().WantTextInput,
                                m_regionPicker.active(),
                                regionInteracting(),
                                framebufferWidth,
                                framebufferHeight,
                                captureStatus};
    const bool drawing = frameWorthDrawing(m_clocks.redrawInputs(signals, glfwGetTime()));
    if (drawing) {
        drawFrame(framebufferWidth, framebufferHeight);
    }

    // The blocking overlay runs after the frame is submitted; capture and
    // analysis keep flowing underneath. These run whether or not a frame was
    // drawn: a region border grabbed while the loop is quiet reaches the
    // application through this poll and no other way.
    applyRegionPickOutcome(m_regionPicker.openIfRequested(m_analysis.region.has_value()));
    applyBorderEditOutcome(m_regions.pollBorderEdit(m_activeWindowIdentity));
    applyRegionPickOutcome(m_regionPicker.poll(m_frameSize, m_cursor.screenSampleColor()));
    commitAnalysisChanges(drawing);
}

// Builds one frame and presents it.
void App::drawFrame(int framebufferWidth, int framebufferHeight)
{
    if (!m_graphics->beginFrame(framebufferWidth, framebufferHeight)) {
        return;
    }

    m_clocks.noteFrameBegun(glfwGetTime());
    if (m_worker.fetchOutput(m_outputVersion, m_output)) {
        m_panes->uploadVisibleScopes(m_analysis.region.has_value());
        SS_DIAG(Perf, "pass analysis_ms=%.1f", m_output.accumulateMilliseconds);
        m_clocks.noteActivity(glfwGetTime());
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

    m_clocks.noteFrameShown(framebufferWidth, framebufferHeight, m_captureController.status());
}

// The user's hand is on the region itself: a rubber band being drawn through
// the picker, or the border being dragged by its band. Both are followed from
// the frame loop, so both take it off its frame period - see frameWaitFor.
bool App::regionInteracting() const
{
    return m_regionPicker.active() || m_regions.borderEditing();
}

void App::serviceCapture(bool framebufferEmpty, double now)
{
    CaptureConditions conditions;
    conditions.visibility =
        VisibilityInputs{m_sessionAsleep.load(),
                         applicationHidden(),
                         glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) != 0,
                         glfwGetWindowAttrib(m_window, GLFW_VISIBLE) != 0,
                         framebufferEmpty,
                         !m_analysis.region.has_value(),
                         m_regionPicker.active() || m_regionPicker.scansRunning() || m_faceLock.probeRunning()};
    conditions.suspended = m_captureController.suspended();
    conditions.frameSize = m_frameSize;
    conditions.region = m_analysis.region;
    conditions.faceLocked = m_faceLock.locked();

    const CaptureDecision decision = m_captureSupervisor.update(conditions, now);
    switch (decision.pipeline) {
    case PipelineAction::Suspend:
        m_captureController.suspend(decision.pauseReason);
        // The frame the worker holds is a whole display of pixels; with the
        // stream stopped nothing will replace it, and the colour readout falls
        // back to the off-stream sample the moment it is gone.
        m_worker.releaseFrame();
        break;
    case PipelineAction::Resume:
        m_captureController.resume();
        break;
    case PipelineAction::Keep:
        break;
    }
    if (decision.cropKnown) {
        m_captureController.narrowTo(decision.crop);
    }
}

void App::pumpEvents()
{
    const double now = glfwGetTime();
    const FrameWaitDecision wait =
        frameWaitFor(m_clocks.pacingInputs(now, m_attach.attached(), m_regionPicker.active(), regionInteracting()));
    switch (wait.kind) {
    case FrameWait::FollowInteraction:
        // Ends on the pointer event that moved the region, so the border is
        // repositioned in the same breath as the hand moved it.
        glfwWaitEventsTimeout(m_clocks.interactionWait(now));
        break;
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
    m_clocks.notePumpReturned(glfwGetTime());
}

void App::drainAsyncSignals()
{
    // First of the drains, and ahead of the capture service below: the focus
    // routing is what takes a stale border down, and everything after this
    // point can stall the tick - a capture restart most of all.
    if (m_callbackState.foregroundChanged.exchange(false)) {
        SS_DIAG(Attach, "fg-event wake");
        followAttachedWindow();
        m_clocks.noteActivity(glfwGetTime());
    }
    if (m_callbackState.displaysChanged.exchange(false)) {
        m_captureController.markStale();
    }
    m_regionPicker.drainFaceScans();
    if (m_callbackState.iconifyChanged.exchange(false)) {
        m_regions.syncBorder(borderState());
        m_clocks.noteActivity(glfwGetTime());
    }
    if (m_orphanEscape.exchange(false)) {
        applyRegionOutcome(m_regions.clearRegion());
        m_clocks.noteActivity(glfwGetTime());
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
        m_clocks.noteActivity(glfwGetTime());
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
                m_clocks.noteActivity(glfwGetTime());
            }
        }
    }
}

void App::syncUiScaleToMonitor()
{
    // The window may have moved to a monitor with a different scale; the user
    // factor rides along through the controller's refresh.
    if (m_uiScale.refresh(m_window)) {
        m_clocks.noteActivity(glfwGetTime());
    }
}

void App::publishSelfWindowMask()
{
    if (!m_frameSize || m_captureController.capturedDisplay() == 0) {
        return;
    }
    const auto geometry = geometryOfDisplay(m_captureController.capturedDisplay());
    if (!geometry) {
        return;
    }
    const IntRect selfWindow = selfWindowMask(windowPlacement(), *geometry, m_frameSize->displayWidth,
                                              m_frameSize->displayHeight, m_uiScale.scale());
    if (!(selfWindow == m_analysis.maskedWindow)) {
        m_analysis.maskedWindow = selfWindow;
        m_analysisDirty = true;
    }
}

void App::notePointerMovement()
{
    const std::optional<DesktopPoint> pointer = globalCursorPosition();
    if (pointer && m_pointerAt && (pointer->x != m_pointerAt->x || pointer->y != m_pointerAt->y)) {
        m_clocks.notePointerMove(glfwGetTime());
    }
    m_pointerAt = pointer;
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
        m_clocks.noteActivity(glfwGetTime());
    }
    if (sample.readoutChanged) {
        m_clocks.noteReadoutActivity(glfwGetTime());
    }
}

void App::updateAdaptiveDetail(int framebufferWidth)
{
    int windowW = 0;
    int windowH = 0;
    glfwGetWindowSize(m_window, &windowW, &windowH);
    const float density = windowW > 0 ? static_cast<float>(framebufferWidth) / static_cast<float>(windowW) : 1.0f;
    const std::optional<DetailSizes> sizes = m_detail.update(m_panes->paneSizes(), density, m_frameSize, glfwGetTime());
    if (!sizes) {
        return;
    }
    // One size per family, put in force for every member: they share a set of
    // bins, and a member at a different size would re-lay them every pass.
    for (const std::string_view id : WaveformFamily) {
        m_analysis.imageSizes[std::string{id}] = sizes->waveform;
    }
    for (const std::string_view id : HistogramFamily) {
        m_analysis.imageSizes[std::string{id}] = sizes->histogram;
    }
    m_analysis.imageSizes[VectorscopeScopeId] = {sizes->vectorscope, sizes->vectorscope};
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
    applyPresetOutcome(m_presetPicker.draw(m_panes->icons()));
    ImGui::SameLine(0.0f, 8.0f);
    applyPaneRenderOutcome(m_panes->drawScopeToggles(modifiers.shift));
    for (const ShortcutAction& action : m_shortcuts.resolvePressed(shortcutContext(), modifiers, shortcutPressed)) {
        applyShortcutAction(action);
    }
    const PaneRenderInput input{m_uiScale.scale(),
                                m_analysis.region.has_value(),
                                anyPinTarget(m_scopeRegistry, m_view.stack().ids()),
                                m_vectorscopeColor,
                                m_waveformColor,
                                m_readoutColor,
                                m_callbackState.monospaceFont};
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
        m_clocks.noteActivity(glfwGetTime());
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
    if (outcome.clearRegion) {
        applyRegionOutcome(m_regions.clearRegion());
    }
    if (outcome.analysisDirty) {
        m_analysisDirty = true;
    }
    if (outcome.activity) {
        m_clocks.noteActivity(glfwGetTime());
    }
    if (outcome.preferencesSaveDue) {
        m_nextPreferencesSave = glfwGetTime() + 1.0;
    }
}

ShortcutContext App::shortcutContext() const
{
    return shortcutContextFor(m_view, m_scopeRegistry, m_showSettings, ImGui::GetIO().WantTextInput);
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
    m_clocks.noteActivity(glfwGetTime());
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
                                 m_quality,
                                 m_analysis.region.has_value()};
    buildContextMenu(model, clickedPane, menu, paramActions);
    const int chosen = showNativeContextMenu(menu);
    dispatchMenuChoice(chosen, paramActions);
}

// Carries out a menu choice. What each id MEANS is the menu's own to say - it
// laid the ranges out - so every branch here is already the action, and what is
// left is the shell state only the host can reach.
void App::dispatchMenuChoice(int chosen, const std::vector<ParamMenuAction>& paramActions)
{
    if (const ParamMenuAction* param = menuScopeParam(chosen, paramActions)) {
        m_analysis.scopeParams[param->scopeId][param->paramKey] = param->value;
        m_analysisDirty = true;
    }
    if (const std::optional<ShortcutAction> action = menuShortcutAction(chosen)) {
        applyShortcutAction(*action);
    }
    if (const std::optional<std::string> scopeId = menuScopeToggle(chosen, m_scopeRegistry)) {
        toggleScope(*scopeId);
    }
    if (const std::optional<float> strength = menuGraticuleStrength(chosen)) {
        m_view.setGraticuleStrength(*strength);
    }
    if (const std::optional<LayoutOrientation> orientation = menuOrientation(chosen)) {
        m_view.layout().setOrientation(*orientation);
    }
    if (const std::optional<int> step = menuUiScaleStep(chosen)) {
        // Only recorded here - the native menu runs inside the host window's
        // WindowPadding push, and selectStep rebuilds the whole style.
        // applyPendingUiScale runs it once that push is popped.
        m_pendingUiScaleStep = *step;
    }
    if (const std::optional<QualityLevel> quality = menuQuality(chosen)) {
        applyQuality(*quality);
    }
    dispatchShellMenu(chosen);
    m_clocks.noteActivity(glfwGetTime());
    m_nextPreferencesSave = glfwGetTime() + 1.0;
}

// The entries that reach no other unit: the diagnostics recorder, the attached
// window, the pin ring, and the About window.
void App::dispatchShellMenu(int chosen)
{
    if (applyDiagnosticsMenu(chosen)) {
        return;
    }
    switch (chosen) {
    case MenuDetachWindow:
        detachActiveWindow();
        break;
    case MenuClearPinnedMarkers:
        m_pins.clear();
        break;
    case MenuAbout:
        m_about.open();
        break;
    default:
        break;
    }
}

void App::applyQuality(QualityLevel level)
{
    m_quality = level;
    const QualityProfile& profile = profileFor(level);
    // The resolutions travel by the detail policy's own debounced route, so a
    // change lands once rather than per frame; the sample rate and the capture
    // cadence are applied here and now.
    m_detail.setQuality(level);
    m_analysis.sampleThinning = profile.sampleThinning;
    m_captureController.setFrameRate(profile.captureFramesPerSecond);
    m_analysisDirty = true;
}

void App::applyPendingUiScale()
{
    if (m_pendingUiScaleStep < 0) {
        return;
    }
    m_uiScale.selectStep(m_pendingUiScaleStep, m_window);
    m_pendingUiScaleStep = -1;
}

void App::commitAnalysisChanges(bool drewThisPass)
{
    const double now = glfwGetTime();
    const RegionMotion motion = trackRegionMotion(now);
    if (m_analysisDirty) {
        // The border follows the hand; the scopes follow the frames. A region
        // under the hand takes the loop off the frame period so the border can
        // keep up, and every settings push is a pass the worker owes - even
        // with no new frame, since the pixels under a moved region differ - so
        // pushing at the pointer's rate would run one per pointer event. Riding
        // the drawn frame keeps the scopes at exactly the rate they are shown
        // at. The border is reconciled either way, which is the whole point of
        // the arrangement.
        m_regions.syncBorder(borderState());
        if (regionInteracting() && !drewThisPass) {
            return;  // still dirty: the pass that draws carries it out
        }
        // Coarse only on the way out: the settings themselves stay the truth,
        // so the detail policy and the projections keep reading what the region
        // will be analysed at the moment it stops moving.
        const bool coarsen = motion == RegionMotion::Dragged && profileFor(m_quality).coarsenWhileDragged;
        m_worker.updateSettings(coarsen ? coarsenedForDrag(m_analysis) : m_analysis);
        m_panes->configureProjections();
        m_analysisDirty = false;
        m_clocks.noteActivity(now);
        m_nextPreferencesSave = now + 1.0;
    }
    if (m_nextPreferencesSave > 0.0 && now > m_nextPreferencesSave) {
        persistPreferences();
        m_nextPreferencesSave = -1.0;
    }
}

// What is moving the region decides what analysis does about it, and the causes
// want different things. A region SCANNED across a picture - a border dragged by
// its band, a rectangle still being drawn - is being read while it moves, so the
// pass goes on at a coarser image. A region THROWN from one face to another is
// not: for the second it is in the air the user is watching the border and
// nothing else, so analysis is held exactly as it is for a window carrying one
// across the desktop. Both releases restore the sharp trace by the same route,
// the settle bumping the settings version.
RegionMotion App::trackRegionMotion(double now)
{
    bool regionChanged = false;
    double travel = 0.0;
    if (m_analysisDirty && m_analysis.region != m_lastSentRegion) {
        travel = regionTravelPercent(m_lastSentRegion, m_analysis.region);
        m_lastSentRegion = m_analysis.region;
        regionChanged = true;
    }

    // A live picker means the user's hand is on the region, whatever a window
    // was doing a moment ago - and the flag it would otherwise carry cannot
    // clear while the picker is up, because the follow step that clears it is
    // suppressed for the picker's whole duration.
    const bool carried = m_attachedWindowMoving && !m_regionPicker.active();
    const RegionMotionStep step = m_motion.update({regionChanged, carried, now, travel});
    if (step.changed) {
        m_worker.hold(step.motion == RegionMotion::Carried || step.thrown);
        m_analysisDirty = true;
    }

    return step.motion;
}

}  // namespace sidescopes
