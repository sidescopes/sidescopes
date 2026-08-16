#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/about_window.h"
#include "app/adaptive_detail.h"
#include "app/attach_controller.h"
#include "app/capture_controller.h"
#include "app/capture_supervisor.h"
#include "app/cursor_sampler.h"
#include "app/face_lock_controller.h"
#include "app/frame_pacing.h"
#include "app/frame_timer.h"
#include "app/layout_preset_picker.h"
#include "app/layout_presets.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/quality.h"
#include "app/region_coordinator.h"
#include "app/region_motion.h"
#include "app/region_picker.h"
#include "app/scope_pane_renderer.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"
#include "app/ui_scale.h"
#include "app/version.h"
#include "app/window_place.h"
#include "core/analysis_worker.h"
#include "core/diagnostics.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "core/preferences.h"
#include "imgui.h"
#include "platform/desktop.h"
#include "platform/graphics.h"
#include "platform/native_menu.h"
#include "platform/region_selection.h"
#include "platform/screen_capture.h"
#include "sidescopes/module.h"

struct GLFWwindow;

namespace sidescopes {

/// State the GLFW C callbacks reach through the window user pointer, since C
/// callbacks cannot capture. One instance lives in the App for the whole run,
/// so a raw pointer to it outlives every callback GLFW fires. The ImGui GLFW
/// backend deliberately leaves the user pointer alone, so it is ours.
struct AppCallbackState
{
    /// Minimizing is "get out of my way": the region border follows the window
    /// down and returns on restore. The flag wakes the frame loop's border sync
    /// when the iconified state flips either way. Set by the GLFW iconify
    /// callback.
    std::atomic<bool> iconifyChanged{false};
    /// A foreground application switch happened: the focus routing must run
    /// now rather than at the next scheduled tick, or the border outlives the
    /// window it dresses. Set by the platform foreground observer, which may
    /// deliver from an operating-system callback context, so the flag is all
    /// the callback touches; the frame loop drains it and does the routing.
    std::atomic<bool> foregroundChanged{false};
    /// A monitor was connected or disconnected. That is evidence the reason a
    /// capture could not be established may have ended, so the frame loop
    /// drains this into the stale mark and the stream is rebuilt promptly
    /// rather than after whatever wait the failures before it had earned. Set
    /// by the GLFW monitor callback.
    std::atomic<bool> displaysChanged{false};
    /// When a window event from the user last arrived: pointer, wheel, key or
    /// focus. The frame loop draws only while the picture can still be
    /// changing, and every hover highlight, tooltip and text cursor in the
    /// interface is a consequence of one of these, so an event is what says
    /// there are frames left to do. Plain rather than atomic because GLFW
    /// delivers input callbacks from the event pump on the main thread, which
    /// is also the only reader.
    double lastInputEvent{0.0};
    /// The fixed-width companion for values whose glyphs must align - hex codes
    /// most of all; null when no system monospace font was found, and the
    /// interface font stands in. Set once at font load, read by the picker.
    ImFont* monospaceFont{nullptr};
};

/// The SideScopes application shell, shared by every platform: a compact,
/// always-on-top window stacking the enabled scopes. All analysis lives in the
/// core library on its own thread; this owns the interaction model (gestures,
/// native menu, region selection) and preferences, while rendering and window
/// chrome live behind the graphics seam. main() constructs one, runs it, and
/// tears it down.
class App
{
public:
    App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// Brings up GLFW, the window, ImGui, capture, and the scope state from
    /// preferences. @return Whether startup succeeded; on failure the shell is
    /// already torn back down to the point it reached.
    [[nodiscard]] bool init();

    /// Runs the frame loop until the window is asked to close.
    void run();

    /// Persists preferences and releases every resource init() acquired.
    void shutdown();

private:
    // --- startup ---
    void setupCapture();

    /// Registers the system observers the frame loop reacts to: sleep and
    /// wake, an Escape with no key window, and foreground changes.
    void observeSystemEvents();

    // --- state accessors ---
    /// Where the window sits, as the toolkit reports it.
    [[nodiscard]] WindowPlacement windowPlacement() const;
    [[nodiscard]] std::optional<uint32_t> displayOfWindow() const;
    /// Creates the regular global region a new session begins with, on the
    /// application window's display and beside the window where space allows.
    void initializeStarterRegion();
    void refreshActivatedScope(std::string_view id);
    void toggleScope(std::string_view id);
    void chooseScope(std::string_view id, bool stack);
    /// The shell state a border sync reads, gathered fresh per call.
    [[nodiscard]] RegionBorderState borderState() const;
    /// Applies a region decision to host state: the region the coordinator
    /// settled on becomes the analysis region, a wholesale detach drops the
    /// active window along with its motion watch, and interaction stamps the
    /// activity clock.
    void applyRegionOutcome(const RegionOutcome& outcome);

    // --- attached regions ---
    void followAttachedWindow();
    [[nodiscard]] std::vector<AttachedWindowObservation> gatherAttachedObservations() const;
    [[nodiscard]] bool activeWindowMoved(const AttachDecision& decision) const;
    void captureActiveDisplay(const AttachDecision& decision);
    void applyAttachDecision(const AttachDecision& decision);
    void refreshAttachedLabel(const AttachDecision& decision);
    [[nodiscard]] std::optional<uint64_t> resolveFocusedWindow() const;
    void onWindowMotion(WindowMotionSignal signal);
    void idleWaitWatchingAttachedWindow();
    void detachActiveWindow();
    /// Attaches or draws a region the picker confirmed: a window click, an
    /// in-attach-mode draw, a face suggestion, or a freehand global draw. Fed
    /// from a RegionPickOutcome by applyRegionPickOutcome.
    void confirmPickedRegion(const ConfirmedPick& pick);
    void adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region);
    void dismissEditedBorder();
    void toggleRegionAttach();
    void attachGlobalRegionToWindow();
    void applyBorderEdit(const RegionOfInterest& edited);

    // --- face locks ---
    /// Applies a controller outcome to host state: an adopted region becomes
    /// the analysis region; a lost lock detaches its window and falls back to
    /// the global region. The controller has already updated its own state.
    void applyFaceLockOutcome(const FaceLockOutcome& outcome);
    bool adoptFacePick(uint32_t displayId, const RegionOfInterest& confirmed);

    static void logAttachMapping(const RegionPicker::WindowCandidate& picked, const RegionOfInterest& start);
    void persistPreferences();

    /// Notes that the worker published a pass, and ends whatever wait the
    /// frame loop is in. Called from the worker's thread.
    void noteWorkerOutput();

    // --- per-frame ---
    void runFrame();
    /// Builds one frame and presents it. Skipped outright while nothing can
    /// have changed what is on screen: a presented frame is what keeps the
    /// graphics driver's per-process render arena resident, and it releases
    /// most of it about a second after the last one.
    void drawFrame(int framebufferWidth, int framebufferHeight);
    /// Whether the user is drawing or dragging the region itself, which takes
    /// the loop off its frame period so the border can follow their hand.
    [[nodiscard]] bool regionInteracting() const;
    /// Gathers what the capture's state is decided from and carries the answer
    /// out: suspending the whole pipeline behind the stream while the window is
    /// out of sight, and narrowing the stream to the region.
    /// @p framebufferEmpty is the frame's own measurement, taken once and read
    /// here and by the draw that follows.
    void serviceCapture(bool framebufferEmpty, double now);
    void pumpEvents();
    void drainAsyncSignals();
    void followWindowDisplay();
    void syncUiScaleToMonitor();
    void publishSelfWindowMask();
    /// Notes whether the pointer has moved since the last pass of the loop.
    ///
    /// Taken outside the frame, because it is one of the things that decides
    /// whether there should be one: the colour readout and both trace markers
    /// are readings of the point under the pointer, and the pointer crosses a
    /// still picture in another application's window without changing the
    /// screen or sending this window an event.
    void notePointerMovement();
    void sampleCursorColor();
    /// Puts the resolutions the adaptive detail settled on in force, so the
    /// worker recomputes each scope's image at the size its pane now wants.
    /// The decision itself is the controller's; only the settings are ours.
    void updateAdaptiveDetail(int framebufferWidth);

    // --- frame UI ---
    void drawFrameUi();
    static void beginHostWindow();
    /// Applies an interface-size step chosen from the context menu, deferred to
    /// after the host window's PopStyleVar. Applying it inside that push - where
    /// the menu runs - rebuilds the whole style, and the matching pop then
    /// restores the pre-change WindowPadding over it. A no-op when none is due.
    void applyPendingUiScale();
    /// Applies a pane-render outcome to host state: a chosen scope comes on
    /// screen, the reset tool drops every region, and the clocks the shell
    /// shares are stamped. The renderer has already drawn the frame and driven
    /// its own collaborators.
    void applyPaneRenderOutcome(const PaneRenderOutcome& outcome);
    /// The shell state a shortcut resolution reads, gathered fresh per call.
    [[nodiscard]] ShortcutContext shortcutContext() const;
    /// Carries out a resolved shortcut: the resolver has already decided what
    /// the key means. Shared with the menu entries driving the same actions.
    void applyShortcutAction(const ShortcutAction& action);
    /// Applies a preset outcome to host state: the strip carries what the
    /// action has to say, and the worker and the preferences file catch up
    /// with the layout it put on screen.
    void applyPresetOutcome(const LayoutPresetOutcome& outcome);
    /// Shows @p message in the status strip and keeps the frame loop awake
    /// while it is up.
    void setStatus(std::string message);

    // --- context menu ---
    void handleContextMenu();
    void dispatchMenuChoice(int chosen, const std::vector<ParamMenuAction>& paramActions);
    /// The menu entries that reach no unit but the shell itself.
    void dispatchShellMenu(int chosen);

    /// Puts @p level in force: the resolutions the detail policy asks for, how
    /// thinly the region is sampled, and how often the screen is read.
    void applyQuality(QualityLevel level);

    // --- post-render region handling ---
    /// Applies a RegionPickOutcome to host state: a live preview becomes the
    /// analysis region (its own no-op check deciding whether anything moved), a
    /// sampled colour joins the pin board, a confirmed pick is attached or
    /// drawn, an Esc cancel clears the region, and the pick's end re-syncs the
    /// region border.
    void applyRegionPickOutcome(const RegionPickOutcome& outcome);
    /// Carries out the region border's live edit: its close affordances, its
    /// attach toggle, or the drag that moved or resized the region.
    void applyBorderEditOutcome(const RegionBorderEditOutcome& outcome);
    void commitAnalysisChanges(bool drewThisPass);

    /// Notes what is moving the region right now and puts the answer in force -
    /// holding analysis while an attached window carries it, and dirtying the
    /// settings whenever it changes so the detail follows.
    RegionMotion trackRegionMotion(double now);

    GLFWwindow* m_window = nullptr;
    AppCallbackState m_callbackState;
    std::unique_ptr<GraphicsBackend> m_graphics;
    /// Presents each built frame and times its body and present for the perf
    /// diagnostics channel. Built once the graphics backend exists.
    std::unique_ptr<FrameTimer> m_frameTimer;
    VersionInfo m_versionInfo;
    /// Owns the factor the interface is drawn at and the user's size
    /// preference folded into it.
    UiScaleController m_uiScale;
    /// The interface-size step the context menu chose this frame, or -1. Held
    /// until applyPendingUiScale runs it past the host window's PopStyleVar.
    int m_pendingUiScaleStep = -1;
    /// How much of the machine the analysis may spend.
    QualityLevel m_quality = QualityLevel::Standard;

    FrameMailbox m_mailbox;
    AnalysisWorker m_worker;
    std::unique_ptr<ScreenCaptureSource> m_capture;
    CaptureController m_captureController;

    AnalysisSettings m_analysis;
    bool m_analysisDirty = true;

    // Attached regions: the attached-window set, beside which the coordinator
    // holds the single global region the analysis falls back to whenever the
    // focused window has no region of its own. The border hides the instant
    // the active window moves - a polled border trails a fast drag - and
    // returns once the window has sat still for the settle time; the motion
    // watch delivers the grab itself, so the hide precedes the first stale
    // composite at any frame rate.
    AttachController m_attach;
    bool m_attachedWindowMoving = false;
    bool m_attachGripActive = false;
    double m_attachRegionMovedAt = -1.0;
    // The active window the motion watch is bound to (0 = none), its last
    // seen rectangle, and the label its border wears.
    uint64_t m_activeWindowIdentity = 0;
    std::optional<AttachWindowRect> m_attachLastSeenRect;
    std::string m_attachActiveLabel;

    /// Owns the face locks, the detection probe thread, and the content
    /// stability watch; the host applies the outcomes it returns.
    FaceLockController m_faceLock;

    /// Owns the region-picker lifecycle - the overlay, the background display
    /// face scans, and the confirm/pin/preview polling; the host applies the
    /// RegionPickOutcome it returns.
    RegionPicker m_regionPicker;

    /// Owns the global region, the border on screen and its labels, and the
    /// latch a border drag in flight runs on; the host applies the
    /// RegionOutcome it returns.
    RegionCoordinator m_regions;

    int64_t m_ownPid = 0;

    ScopeRegistry m_scopeRegistry;
    ScopeView m_view;

    /// Owns the keyboard bindings and maps a key to the action it means; the
    /// host applies the ShortcutAction it returns.
    ShortcutResolver m_shortcuts;

    bool m_showSettings = false;

    /// Owns whether the About window is on screen and everything it shows.
    AboutWindow m_about;
    PinBoard m_pins;

    /// Owns the color under the pointer - the cross-display sample and the
    /// per-trace smoothing; the host hands what it returns to its drawing.
    CursorSampler m_cursor;

    /// Owns the layout preset slots and what a slot records of - and restores
    /// to - the live layout; the host applies the outcomes it returns.
    LayoutPresetController m_presets;

    /// Draws the toolbar's preset control over those slots, and owns the one
    /// thing the list has of its own: the name being typed into a row.
    LayoutPresetPicker m_presetPicker;

    /// Owns the debounced choice of what resolution each scope's image is
    /// computed at; the host puts the resolutions it settles on in force.
    AdaptiveDetail m_detail;

    uint64_t m_outputVersion = 0;
    AnalysisWorker::Output m_output;

    /// Draws the scopes' side of the window and owns what only that drawing
    /// needs - the textures, the projections the overlays come from, the pane
    /// geometry, and the transient lines; the host applies the
    /// PaneRenderOutcome it returns. Built once the graphics backend exists.
    /// Declared after every member it binds - the pins, the bindings, the
    /// worker output - because members die in reverse declaration order and it
    /// holds them by reference.
    std::unique_ptr<ScopePaneRenderer> m_panes;

    /// Owns when each thing the picture follows last happened, and what the
    /// frame on screen was drawn from; the loop asks it how long to block and
    /// whether to draw.
    FrameClocks m_clocks;
    /// The region the worker was last told about: a region that differs from it
    /// is a region something is moving, and what is moving it decides whether
    /// the scopes go coarse or stop altogether.
    std::optional<RegionOfInterest> m_lastSentRegion;
    /// Owns which of the two that is, and the clock its settle time is
    /// measured against.
    RegionMotionTracker m_motion;
    /// Where the pointer was on the previous pass of the loop. The clock for
    /// when it was last somewhere else is one of the loop's.
    std::optional<DesktopPoint> m_pointerAt;
    /// Owns whether the pipeline should be running and how wide the capture
    /// should be, with the clocks both answers are measured against.
    CaptureSupervisor m_captureSupervisor;
    /// Whether the session has stopped showing anything - the display asleep,
    /// the screen locked, another user switched in. Set from the platform
    /// observers, which may deliver on any thread, and read by the frame loop.
    std::atomic<bool> m_sessionAsleep{false};
    double m_nextPreferencesSave = -1.0;
    std::atomic<bool> m_orphanEscape{false};

    // Recomputed every frame, held on the App only to flow between the phase
    // methods a single frame runs through.
    std::optional<FloatColor> m_vectorscopeColor;
    std::optional<FloatColor> m_waveformColor;
    std::optional<FloatColor> m_readoutColor;
    std::optional<AnalysisWorker::FrameSize> m_frameSize;
};

}  // namespace sidescopes
