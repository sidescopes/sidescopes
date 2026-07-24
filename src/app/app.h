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
#include "app/cursor_sampler.h"
#include "app/face_lock_controller.h"
#include "app/frame_timer.h"
#include "app/layout_presets.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/region_coordinator.h"
#include "app/region_picker.h"
#include "app/scope_pane_renderer.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"
#include "app/ui_scale.h"
#include "app/version.h"
#include "core/analysis_worker.h"
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
    void setupImGui();
    void setupCapture();
    void setupView(const Preferences& startup);

    // --- state accessors ---
    [[nodiscard]] std::optional<uint32_t> displayOfWindow() const;
    [[nodiscard]] double scopeParam(std::string_view id, std::string_view key, double fallback) const;
    [[nodiscard]] bool pinsAvailable() const;
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

    // --- per-frame ---
    void runFrame();
    void pumpEvents();
    void drainAsyncSignals();
    void followWindowDisplay();
    void syncUiScaleToMonitor();
    void publishSelfWindowMask();
    void sampleCursorColor();
    /// Puts the resolutions the adaptive detail settled on in force, so the
    /// worker recomputes each scope's image at the size its pane now wants.
    /// The decision itself is the controller's; only the settings are ours.
    void updateAdaptiveDetail(int framebufferWidth);

    // --- frame UI ---
    void drawFrameUi();
    static void beginHostWindow();
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
    void dispatchScopeToggleMenu(int chosen);
    void dispatchRegionMenu(int chosen);
    void dispatchViewMenu(int chosen);
    void dispatchLayoutMenu(int chosen);
    void dispatchUiScaleMenu(int chosen);

    // --- post-render region handling ---
    /// Applies a RegionPickOutcome to host state: a live preview becomes the
    /// analysis region (its own no-op check deciding whether anything moved), a
    /// sampled colour joins the pin board, a confirmed pick is attached or
    /// drawn, an Esc cancel resets to full screen, and the pick's end re-syncs
    /// the region border.
    void applyRegionPickOutcome(const RegionPickOutcome& outcome);
    /// Carries out the region border's live edit: its close affordances, its
    /// attach toggle, or the drag that moved or resized the region.
    void applyBorderEditOutcome(const RegionBorderEditOutcome& outcome);
    void commitAnalysisChanges();

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

    double m_lastActivity = 0.0;
    double m_nextPreferencesSave = -1.0;
    std::atomic<bool> m_orphanEscape{false};

    // Recomputed every frame, held on the App only to flow between the phase
    // methods a single frame runs through.
    std::optional<FloatColor> m_vectorscopeColor;
    std::optional<FloatColor> m_waveformColor;
    std::optional<AnalysisWorker::FrameSize> m_frameSize;
};

}  // namespace sidescopes
