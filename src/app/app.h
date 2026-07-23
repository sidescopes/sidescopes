#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/attach_controller.h"
#include "app/capture_controller.h"
#include "app/face_lock_controller.h"
#include "app/layout_preset_store.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/region_picker.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/version.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "core/marker_smoother.h"
#include "core/preferences.h"
#include "core/region_kind.h"
#include "core/scopes/histogram.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/desktop.h"
#include "platform/graphics.h"
#include "platform/icons.h"
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
    void applyUiScale(float scale);
    /// Reapplies the interface scale from the current monitor and the user's
    /// size factor. Returns whether it changed. The one path that folds the OS
    /// scale and the preference together, so both the startup and the
    /// monitor-change sites stay in agreement.
    bool refreshUiScale();
    void setupCapture();
    void setupView(const Preferences& startup);

    // --- state accessors ---
    [[nodiscard]] std::optional<uint32_t> displayOfWindow() const;
    [[nodiscard]] double scopeParam(std::string_view id, std::string_view key, double fallback) const;
    [[nodiscard]] std::pair<int, int> currentSize(std::string_view id) const;
    [[nodiscard]] HistogramStyle currentHistogramStyle() const;
    void setWaveformGain(double gain);
    [[nodiscard]] const SsScopeDescriptor* descriptorFor(std::string_view id) const;
    void configureProjectionInstances();
    [[nodiscard]] const ScopeInstance* projectionFor(std::string_view id) const;
    [[nodiscard]] std::string bindingFor(std::string_view id) const;
    [[nodiscard]] bool pinsAvailable() const;
    [[nodiscard]] const ScopeImage& imageForId(std::string_view id) const;
    [[nodiscard]] ScopeTexture& textureForId(std::string_view id);
    void uploadScope(std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image);
    void uploadVisibleScopes();
    void refreshActivatedScope(std::string_view id);
    void toggleScope(std::string_view id);
    void chooseScope(std::string_view id, bool stack);
    /// @return Whether the region the scopes are reading covers the whole
    ///         captured display - the state Watch Full Screen restores. An
    ///         extent, not a kind: a region attached to a window filling the
    ///         display reads full here too.
    [[nodiscard]] bool isFullScreen() const;
    /// @return The kind of the region the scopes are reading right now: the
    ///         active attached window's, or the global one. A narrower
    ///         question than AttachController::attached(), which reports
    ///         whether ANY window holds a region - an attached window that is
    ///         not focused leaves the global kind in effect. Orthogonal to
    ///         isFullScreen(), which measures a region's extent rather than
    ///         what it is bound to.
    [[nodiscard]] RegionKind regionKind() const;
    void syncRegionBorder();
    void setRegion(const RegionOfInterest& region);

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
    void resetToFullScreen();
    void persistPreferences();

    // --- per-frame ---
    void runFrame();
    void pumpEvents();
    /// Stamps the frame-body clock for the perf channel, after the event
    /// wait so an idle wait never counts as frame work. A no-op when perf
    /// diagnostics are off.
    void markFrameBodyStart();
    /// Presents the built frame and, for the perf channel, logs the frame
    /// body and the present/vsync wait as one line. A no-op present wrapper
    /// when perf diagnostics are off.
    void presentFrame();
    void drainAsyncSignals();
    void followWindowDisplay();
    void syncUiScaleToMonitor();
    void publishSelfWindowMask();
    void sampleCursorColor();
    void updateAdaptiveDetail(int framebufferWidth);
    [[nodiscard]] ImVec2 paneSizePixels(std::string_view id, float density) const;
    [[nodiscard]] std::pair<int, int> desiredWaveformSize(float density, int regionWidth) const;
    [[nodiscard]] std::pair<int, int> desiredHistogramSize(float density) const;

    /// A lazily rasterized texture for one of the embedded set's icons,
    /// rebuilt when the requested pixel size changes with the display.
    struct IconTexture
    {
        std::unique_ptr<ScopeTexture> texture;
        int sizePixels = 0;
    };

    [[nodiscard]] ImTextureID iconTextureId(Icon icon, int sizePixels);
    [[nodiscard]] int desiredVectorscopeSize(float density) const;

    // --- frame UI ---
    void drawFrameUi();
    static void beginHostWindow();
    void drawScopeToggles(bool stackModifier);
    void handleShortcuts(const ModifierState& modifiers);
    void handleCommandChords(const ModifierState& modifiers);
    void handleControlChords(const ModifierState& modifiers);
    void handleLetterShortcuts(const ModifierState& modifiers, bool systemChord);
    void handleViewShortcuts();
    bool triggerShortcut(const std::string& key, bool shift);
    void handlePresetShortcuts(const ModifierState& modifiers);
    void loadLayoutPreset(int slot);
    void saveLayoutPreset(int slot);
    [[nodiscard]] std::map<std::string, double> currentStackWeights() const;
    /// Seats the constant-width region toolbox: right-aligned beside the
    /// scopes, flush left on its own wrapped row, attach notice on the left.
    void placeRegionToolbox();
    void drawRegionToolIcons();
    /// The status bar's reserved height below the panes.
    [[nodiscard]] static float statusBarHeight();
    /// The reserved strip under the panes: the pin tool in the left corner,
    /// the live swatch in the right, messages and the readout between them.
    void drawStatusBar();
    /// The bar's colour sampler, anchored to the strip's left corner.
    void drawPinTool();
    /// Draws the swatch inwards from the right corner, preceded by the
    /// channel readout when the room left by \p taken - the row's used width
    /// so far - allows it.
    void drawCursorReadout(float taken);
    void drawScopePanes();
    /// What fills the pane area: the capture help pages, or the scope stack.
    void drawPaneContent();
    void drawScopeStack();
    /// Each stacked scope's preferred pane aspect, in stack order, for the
    /// Automatic split scoring.
    [[nodiscard]] std::vector<float> stackAspects() const;
    /// The stacked scopes' choice-parameter values - the style menus' state -
    /// for a preset to recall alongside the geometry.
    [[nodiscard]] std::map<std::string, std::map<std::string, double>> currentStackStyles() const;
    /// The live layout as it would save into a preset slot.
    [[nodiscard]] LayoutPreset capturePreset() const;
    /// Whether the live layout has drifted from the active preset slot; false
    /// when no preset is active.
    [[nodiscard]] bool activePresetDirty() const;
    /// The toolbar preset dropdown: the preview names the active slot
    /// (starred when dirty); the popup loads on click, saves on Shift+click.
    void drawPresetPicker();
    /// Applies a preset's stored choice values through the same write the
    /// style menus use, skipping keys the descriptors no longer declare and
    /// clamping each value to its parameter's range.
    void applyPresetStyles(const std::map<std::string, std::map<std::string, double>>& styles);
    void drawPaneDivider(int leftPane, bool sideBySide, float thickness, const ImVec2& area,
                         const std::vector<float>& lengths);
    static void paintDivider(bool sideBySide, bool highlighted);
    void adjustDividerWeights(int leftPane, float deltaPixels, const std::vector<float>& lengths);
    void equalizeDividerWeights(int leftPane);
    void drawScopeById(std::string_view id);
    void drawVectorscopePane();
    void drawWaveformPane(std::string_view id);
    void setStatus(std::string message);
    void drawAboutWindow();

    // --- context menu ---
    void handleContextMenu();
    [[nodiscard]] const std::map<std::string, double>& paramsOf(std::string_view id) const;
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
    /// The throttled cross-display cursor sample under its lock, passed to the
    /// picker's pin tool each poll.
    [[nodiscard]] std::optional<FloatColor> currentScreenSampleColor() const;
    void handleRegionBorderEdit();
    void commitAnalysisChanges();

    // The freshest cross-display sample: the async sampler's callback may land
    // on any thread, and may still be in flight at shutdown, so the state it
    // writes is shared ownership.
    struct ScreenSample
    {
        std::mutex mutex;
        std::optional<FloatColor> color;
    };

    GLFWwindow* m_window = nullptr;
    AppCallbackState m_callbackState;
    std::unique_ptr<GraphicsBackend> m_graphics;
    VersionInfo m_versionInfo;
    float m_uiScale = 1.0f;
    /// User interface-size factor, a multiplier on the OS scale (1.0 = match
    /// system). One of UiScaleSteps; multiplied into m_uiScale by refreshUiScale.
    float m_userUiScaleFactor = 1.0f;

    FrameMailbox m_mailbox;
    AnalysisWorker m_worker;
    std::unique_ptr<ScreenCaptureSource> m_capture;
    CaptureController m_captureController;

    AnalysisSettings m_analysis;
    bool m_analysisDirty = true;

    // Attached regions: the attached-window set and the single global region
    // the analysis falls back to whenever the focused window has no region
    // of its own. The border hides the instant the active window moves - a
    // polled border trails a fast drag - and returns once the window has sat
    // still for the settle time; the motion watch delivers the grab itself,
    // so the hide precedes the first stale composite at any frame rate.
    AttachController m_attach;
    RegionOfInterest m_globalRegion;
    bool m_attachedWindowMoving = false;
    bool m_attachGripActive = false;
    double m_attachRegionMovedAt = -1.0;
    // The active window the motion watch is bound to (0 = none), its last
    // seen rectangle, and the label its border wears.
    uint64_t m_activeWindowIdentity = 0;
    std::optional<AttachWindowRect> m_attachLastSeenRect;
    std::string m_attachActiveLabel;
    // Which region the border edit in flight started on (0 = the global
    // one), latched at the drag's first frame: no focus race can reroute a
    // grabbed border, and an attached edit can never convert to global.
    bool m_attachBorderEditing = false;
    uint64_t m_attachBorderEditIdentity = 0;

    /// The global region's border label: the captured display's name,
    /// refreshed when the captured display changes.
    std::string m_displayLabel;
    uint32_t m_displayLabelId = 0;

    /// Owns the face locks, the detection probe thread, and the content
    /// stability watch; the host applies the outcomes it returns.
    FaceLockController m_faceLock;

    /// Owns the region-picker lifecycle - the overlay, the background display
    /// face scans, and the confirm/pin/preview polling; the host applies the
    /// RegionPickOutcome it returns.
    RegionPicker m_regionPicker;

    std::array<IconTexture, IconCount> m_iconTextures;
    // A short-lived toolbar note after an attached window closed.
    std::string m_attachDetachNotice;
    double m_attachNoticeUntil = 0.0;
    int64_t m_ownPid = 0;

    ScopeRegistry m_scopeRegistry;
    ScopeView m_view;
    TraceFlash m_flash;
    std::map<std::string, ScopeInstance> m_projectionInstances;

    ShortcutBindings m_shortcuts;
    std::map<std::string, std::string> m_scopeShortcuts;

    bool m_showSettings = false;
    bool m_showAbout = false;
    PinBoard m_pins;

    MarkerSmoother m_vectorscopeMarker;
    MarkerSmoother m_waveformMarker;

    LayoutPresetStore m_presetStore;
    std::string m_statusMessage;
    double m_statusUntil = 0.0;

    std::map<std::string, std::unique_ptr<ScopeTexture>> m_scopeTextures;
    std::vector<ImVec2> m_panePoints;
    std::vector<std::string> m_paneIds;
    std::vector<std::string> m_dividerIds;
    int m_pendingColumns = 0;
    int m_pendingImageHeight = 0;
    int m_pendingVectorscope = 0;
    int m_pendingHistWidth = 0;
    int m_pendingHistHeight = 0;
    double m_detailPendingSince = 0.0;

    uint64_t m_outputVersion = 0;
    AnalysisWorker::Output m_output;

    // Perf-channel frame timing: the frame body's start, stamped only while
    // the channel records; the flag marks the stamp fresh for this frame so
    // a mid-frame record toggle never times against a stale start.
    std::chrono::steady_clock::time_point m_frameBodyStart;
    bool m_frameBodyStamped = false;
    double m_lastActivity = 0.0;
    double m_nextPreferencesSave = -1.0;
    DesktopPoint m_lastCursor{-1.0, -1.0};

    std::shared_ptr<ScreenSample> m_screenSample;
    double m_nextScreenSample = 0.0;
    std::atomic<bool> m_orphanEscape{false};
    std::vector<ImVec2> m_histogramScratch;

    // Recomputed every frame, held on the App only to flow between the phase
    // methods a single frame runs through.
    std::optional<FloatColor> m_vectorscopeColor;
    std::optional<FloatColor> m_waveformColor;
    std::optional<AnalysisWorker::FrameSize> m_frameSize;
    std::vector<ImVec4> m_paneRects;
};

}  // namespace sidescopes
