#pragma once

#include <atomic>
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
#include "app/face_pin.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/version.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "core/marker_smoother.h"
#include "core/preferences.h"
#include "core/scopes/histogram.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/desktop.h"
#include "platform/graphics.h"
#include "platform/native_menu.h"
#include "platform/region_selection.h"
#include "platform/screen_capture.h"
#include "sidescopes/module.h"

struct GLFWwindow;

namespace sidescopes {

/// State the GLFW C callbacks and the background face-check worker reach
/// through the window user pointer, since C callbacks cannot capture. One
/// instance lives in the App for the whole run, so a raw pointer to it
/// outlives every callback GLFW fires and every thread it spawns. The ImGui
/// GLFW backend deliberately leaves the user pointer alone, so it is ours.
struct AppCallbackState
{
    /// How many faces the platform detector saw on the captured screen at the
    /// last check: -1 before any check. Refreshed in the background when the
    /// application gains focus, so the face button can present itself honestly
    /// - dimmed when there is currently nothing to pick. The state can go stale
    /// while the user works elsewhere, so the button only dims, never disables:
    /// pressing F always detects freshly. Written by the async worker thread
    /// and read by the UI: stays atomic.
    std::atomic<int> facesOnScreen{-1};
    /// Set by the GLFW focus callback, drained by the frame loop.
    std::atomic<bool> faceCheckRequested{false};
    /// Guards a single in-flight async check; shared by the frame loop and the
    /// worker thread, so it stays atomic.
    std::atomic<bool> faceCheckRunning{false};
    /// Minimizing is "get out of my way": the region border follows the window
    /// down and returns on restore. The flag wakes the frame loop's border sync
    /// when the iconified state flips either way. Set by the GLFW iconify
    /// callback.
    std::atomic<bool> iconifyChanged{false};
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
    [[nodiscard]] bool createMainWindow(const Preferences& startup);
    void setupImGui();
    void applyUiScale(float scale);
    void setupCapture();
    void seedAnalysis(const Preferences& startup);
    void setupView(const Preferences& startup);
    void createProjectionInstances();
    void createScopeTextures();
    [[nodiscard]] std::unique_ptr<ScopeTexture> createBlankTexture(int width, int height);

    // --- state accessors ---
    [[nodiscard]] std::optional<uint32_t> displayOfWindow() const;
    [[nodiscard]] double scopeParam(std::string_view id, std::string_view key, double fallback) const;
    [[nodiscard]] std::pair<int, int> currentSize(std::string_view id) const;
    [[nodiscard]] HistogramStyle currentHistogramStyle() const;
    void setWaveformGain(double gain);
    void setWaveformStride(int stride);
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
    [[nodiscard]] bool isFullRegion() const;
    void syncRegionBorder();
    void setRegion(const RegionOfInterest& region);

    // --- attached regions ---
    void probeExternalWindow();
    void followAttachedWindow();
    [[nodiscard]] std::vector<TrackedWindowObservation> gatherTrackedObservations() const;
    [[nodiscard]] bool activeWindowMoved(const AttachDecision& decision) const;
    void captureActiveDisplay(const AttachDecision& decision);
    void applyAttachDecision(const AttachDecision& decision);
    void refreshAttachedLabel(const AttachDecision& decision);
    void onWindowMotion(WindowMotionSignal signal);
    void idleWaitWatchingAttachedWindow();
    [[nodiscard]] std::optional<PickConstraint> makeAttachedDrawConstraint();
    [[nodiscard]] static RegionOfInterest displayPercentRect(const WindowGeometry& windowGeom,
                                                             const DisplayGeometry& display);
    void stopTrackingActiveWindow();
    void confirmPickedRegion(const RegionPickPoll& poll);
    void adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region);
    void dismissEditedBorder();
    void applyBorderEdit(const RegionOfInterest& edited);

    // --- face pins ---
    /// A pin plus the window rectangle it last saw: a same-size window
    /// move translates the pin's anchors along with the face.
    struct AppFacePin
    {
        FacePinState state;
        std::optional<AttachWindowRect> windowRect;
    };

    void updateFacePin(const AttachDecision& decision);
    void launchFacePinProbe(const AttachDecision& decision, const FacePinState& pin);
    void consumeFacePinProbe(const AttachDecision& decision);
    void applyFacePinRegion(const FacePinState& pin);
    void trackPinWindowRect(AppFacePin& pin, const AttachWindowRect& rect);
    void removeLostFacePin(uint64_t identity);
    void probeRegionContentChange();
    [[nodiscard]] bool regionContentUnsettled() const;
    bool adoptFacePick(uint32_t displayId, const RegionOfInterest& confirmed);
    void logFacePin(const std::string& line);

    /// A window the picker offered, remembered with its identity so a
    /// confirmed window pick can be turned into an attachment.
    struct PickableWindow
    {
        uint64_t identity = 0;
        int64_t ownerPid = 0;
        std::string application;
        AttachWindowRect windowRect;
        RegionOfInterest region;
        uint32_t displayId = 0;
    };

    [[nodiscard]] const PickableWindow* matchPickedWindow(uint32_t displayId, const RegionOfInterest& region) const;

    /// A face the picker offered, remembered with its detector box and the
    /// frame it was measured on so a confirmed face pick can be turned into
    /// a pinned attachment.
    struct PickableFace
    {
        RegionOfInterest region;
        IntRect box;
        uint32_t displayId = 0;
        int frameWidth = 0;
        int frameHeight = 0;
    };

    [[nodiscard]] const PickableFace* matchPickedFace(uint32_t displayId, const RegionOfInterest& region) const;
    [[nodiscard]] const PickableWindow* windowContaining(uint32_t displayId, const RegionOfInterest& region) const;
    [[nodiscard]] std::optional<FloatColor> averageFrameArea(const RegionOfInterest& area) const;
    void resetRegionToFull();
    void persistPreferences();

    // --- per-frame ---
    void runFrame();
    void pumpEvents();
    void drainAsyncSignals();
    void followWindowDisplay();
    void syncUiScaleToMonitor();
    void publishSelfWindowMask();
    void sampleCursorColor();
    void updateAdaptiveDetail(int framebufferWidth);
    [[nodiscard]] ImVec2 paneSizePixels(std::string_view id, float density) const;
    [[nodiscard]] std::pair<int, int> desiredWaveformSize(float density, int regionWidth) const;
    [[nodiscard]] std::pair<int, int> desiredHistogramSize(float density) const;
    [[nodiscard]] int desiredVectorscopeSize(float density) const;

    // --- frame UI ---
    void drawFrameUi();
    void beginHostWindow();
    void drawScopeToggles(bool stackModifier);
    void handleShortcuts(const ModifierState& modifiers);
    void handleCommandChords(const ModifierState& modifiers);
    void handleControlChords(const ModifierState& modifiers);
    void handleLetterShortcuts(const ModifierState& modifiers, bool systemChord);
    void handleViewShortcuts();
    bool triggerShortcut(const std::string& key, bool shift);
    void drawRegionToolIcons();
    void drawCursorReadout();
    void drawScopePanes();
    void drawScopeById(std::string_view id);
    void drawVectorscopePane();
    void drawWaveformPane(std::string_view id);
    void drawSettingsWindow();
    void drawVectorscopeSettings();
    void drawWaveformSettings();
    void drawAboutWindow();

    // --- context menu ---
    void handleContextMenu();
    [[nodiscard]] const std::map<std::string, double>& paramsOf(std::string_view id) const;
    [[nodiscard]] bool scopeHasOptions(std::string_view id) const;
    void appendPinOptions(std::vector<NativeMenuItem>& menu);
    void appendZoomOptions(std::vector<NativeMenuItem>& menu);
    void appendScopeOptions(std::string_view id, bool flatten, std::vector<NativeMenuItem>& menu,
                            std::vector<ParamMenuAction>& paramActions);
    void appendScopesSubmenu(std::vector<NativeMenuItem>& menu);
    void appendPerScopeOptions(std::vector<NativeMenuItem>& menu, std::vector<ParamMenuAction>& paramActions);
    void appendRegionAndAppSection(std::vector<NativeMenuItem>& menu);
    void buildContextMenu(int clickedPane, std::vector<NativeMenuItem>& menu,
                          std::vector<ParamMenuAction>& paramActions);
    void dispatchMenuChoice(int chosen, const std::vector<ParamMenuAction>& paramActions);
    void dispatchScopeToggleMenu(int chosen);
    void dispatchRegionMenu(int chosen);
    void dispatchViewMenu(int chosen);

    // --- post-render region handling ---
    void handleRegionPicking();
    void openRegionPicker();
    void waitForBorderFreeFrame();
    [[nodiscard]] std::vector<PickerDisplay> buildPickerDisplays();
    void dumpSuggestionsIfRequested(const std::vector<PickerDisplay>& pickerDisplays);
    void handleRegionBorderEdit();
    void pollActiveRegionPick();
    void pollPinPick(const RegionPickPoll& poll);
    void applyPinnedColor(const RegionPickPoll& poll);
    void pollRegionPreview(const RegionPickPoll& poll);
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

    FrameMailbox m_mailbox;
    AnalysisWorker m_worker;
    std::unique_ptr<ScreenCaptureSource> m_capture;
    std::optional<CaptureController> m_captureController;

    AnalysisSettings m_analysis;
    bool m_analysisDirty = true;

    // Attached regions: the tracked-window set and the single global region
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
    uint64_t m_attachActiveWatched = 0;
    std::optional<AttachWindowRect> m_attachLastSeenRect;
    std::string m_attachActiveLabel;
    // Which region the border edit in flight started on (0 = the global
    // one), latched at the drag's first frame: no focus race can reroute a
    // grabbed border, and an attached edit can never convert to global.
    bool m_attachBorderEditing = false;
    uint64_t m_attachBorderEditTarget = 0;

    // The attached-draw gesture (Shift+D): the externally focused window
    // SideScopes last stole focus from, and the target carried from the
    // pick's opening to its confirm.
    struct AttachedDrawTarget
    {
        uint64_t identity = 0;
        int64_t ownerPid = 0;
        std::string application;
    };

    std::optional<AttachedDrawTarget> m_attachedDrawTarget;
    std::vector<PickableWindow> m_pickableWindows;
    std::vector<PickableFace> m_pickableFaces;

    /// The face-probe mailbox: a detached detection thread fills it, the
    /// main loop drains it; at most one probe is in flight.
    struct FacePinProbe
    {
        std::atomic<bool> running{false};
        std::atomic<bool> ready{false};
        std::mutex mutex;
        std::vector<IntRect> faces;  ///< detector boxes, full-frame pixels
        uint64_t forWindow = 0;
        IntRect roi;  ///< the searched area, for judging edge-clipped boxes
    };

    std::map<uint64_t, AppFacePin> m_facePins;
    FacePinProbe m_facePinProbe;
    double m_nextFacePinProbe = 0.0;
    /// True while the active pin's face is not confirmed where the region
    /// sits; the border hides instead of outlining stale content.
    bool m_facePinHunting = false;
    /// The content-stability probe over the pinned region: a sparse pixel
    /// grid compared frame to frame; the border shows only settled content.
    std::vector<uint8_t> m_regionContentSamples;
    RegionOfInterest m_regionContentRect;
    double m_regionContentChangedAt = -1.0;
    uint64_t m_lastExternalWindowId = 0;
    int64_t m_lastExternalOwnerPid = 0;
    double m_nextExternalWindowProbe = 0.0;
    bool m_wantAttachedDraw = false;
    // A short-lived toolbar note after a tracked window closed.
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

    bool m_regionPicking = false;
    bool m_regionPickIsPin = false;
    bool m_regionPickSwallowCancel = false;

    MarkerSmoother m_vectorscopeMarker;
    MarkerSmoother m_waveformMarker;

    std::map<std::string, std::unique_ptr<ScopeTexture>> m_scopeTextures;
    std::vector<ImVec2> m_panePoints;
    std::vector<std::string> m_paneIds;
    int m_pendingColumns = 0;
    int m_pendingImageHeight = 0;
    int m_pendingVectorscope = 0;
    int m_pendingHistWidth = 0;
    int m_pendingHistHeight = 0;
    double m_detailPendingSince = 0.0;

    uint64_t m_outputVersion = 0;
    AnalysisWorker::Output m_output;
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
    std::optional<RegionPickerMode> m_wantRegionPick;
    std::optional<AnalysisWorker::FrameSize> m_frameSize;
    std::vector<ImVec4> m_paneRects;
};

}  // namespace sidescopes
