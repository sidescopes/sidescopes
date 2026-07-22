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
#include "app/face_lock.h"
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
    void followAttachedWindow();
    [[nodiscard]] std::vector<AttachedWindowObservation> gatherAttachedObservations() const;
    [[nodiscard]] bool activeWindowMoved(const AttachDecision& decision) const;
    void captureActiveDisplay(const AttachDecision& decision);
    void applyAttachDecision(const AttachDecision& decision);
    void refreshAttachedLabel(const AttachDecision& decision);
    [[nodiscard]] std::optional<uint64_t> resolveFocusedWindow() const;
    void onWindowMotion(WindowMotionSignal signal);
    void idleWaitWatchingAttachedWindow();
    [[nodiscard]] static RegionOfInterest displayPercentRect(const WindowGeometry& windowGeom,
                                                             const DisplayGeometry& display);
    void detachActiveWindow();
    void confirmPickedRegion(const RegionPickPoll& poll);
    void adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region);
    void dismissEditedBorder();
    void toggleRegionAttach();
    void attachGlobalRegionToWindow();
    void applyBorderEdit(const RegionOfInterest& edited);

    // --- face locks ---
    /// A lock plus the window rectangle it last saw: a same-size window
    /// move translates the lock's anchors along with the face. The verdict
    /// stamp says when a probe (or the creating pick) last confirmed the
    /// anchor; an anchor older than a probe period is stale on activation
    /// and must not dress the border until re-verified.
    struct AppFaceLock
    {
        FaceLockState state;
        std::optional<AttachWindowRect> windowRect;
        double anchorVerifiedAt = 0.0;
    };

    void updateFaceLock(const AttachDecision& decision);
    void launchFaceLockProbe(const AttachDecision& decision, const FaceLockState& lock);
    void consumeFaceLockProbe(const AttachDecision& decision);
    void applyFaceLockRegion(const FaceLockState& lock);
    void carryLockWithWindow(AppFaceLock& lock, const AttachWindowRect& rect);
    void removeLostFaceLock(uint64_t identity);
    void probeRegionContentChange();
    [[nodiscard]] bool regionContentUnsettled() const;
    bool adoptFacePick(uint32_t displayId, const RegionOfInterest& confirmed);

    /// What the application keeps for one window the picker suggested, so a
    /// confirmed pick resolves back to its source: the rectangle the match
    /// runs against, and the identity the attachment is made on.
    struct WindowCandidate
    {
        uint64_t identity = 0;
        int64_t ownerPid = 0;
        std::string application;
        AttachWindowRect windowRect;
        RegionOfInterest region;
        uint32_t displayId = 0;
    };

    [[nodiscard]] const WindowCandidate* matchWindowCandidate(uint32_t displayId, const RegionOfInterest& region) const;
    [[nodiscard]] const FaceCandidate* matchFaceCandidate(uint32_t displayId, const RegionOfInterest& region) const;
    void logAttachMapping(const WindowCandidate& picked, const RegionOfInterest& start) const;
    [[nodiscard]] const WindowCandidate* windowContaining(uint32_t displayId, const RegionOfInterest& region) const;
    [[nodiscard]] std::optional<FloatColor> averageFrameArea(const RegionOfInterest& area) const;
    void resetRegionToFull();
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
    void beginHostWindow();
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
    [[nodiscard]] float statusBarHeight() const;
    /// The readout's full width in the bar, for the message-overlap yield.
    [[nodiscard]] float cursorReadoutWidth() const;
    /// The reserved strip under the panes: transient status on the left, the
    /// live readout on the right.
    void drawStatusBar();
    void drawCursorReadout();
    void drawScopePanes();
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
    void paintDivider(bool sideBySide, bool highlighted);
    void adjustDividerWeights(int leftPane, float deltaPixels, const std::vector<float>& lengths);
    void equalizeDividerWeights(int leftPane);
    void drawScopeById(std::string_view id);
    void drawVectorscopePane();
    void drawWaveformPane(std::string_view id);
    void setStatus(std::string message);
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
    void appendLayoutSubmenu(std::vector<NativeMenuItem>& menu);
    void appendPresetsSubmenu(std::vector<NativeMenuItem>& menu);
    void appendRegionAndAppSection(std::vector<NativeMenuItem>& menu);
    void buildContextMenu(int clickedPane, std::vector<NativeMenuItem>& menu,
                          std::vector<ParamMenuAction>& paramActions);
    void dispatchMenuChoice(int chosen, const std::vector<ParamMenuAction>& paramActions);
    void dispatchScopeToggleMenu(int chosen);
    void dispatchRegionMenu(int chosen);
    void dispatchViewMenu(int chosen);
    void dispatchLayoutMenu(int chosen);

    // --- post-render region handling ---
    void handleRegionPicking();
    void openRegionPicker();
    void waitForBorderFreeFrame();
    struct DisplayFaceScan;
    [[nodiscard]] std::vector<PickerDisplay> buildPickerDisplays();
    void launchDisplayFaceScans(const std::vector<PickerDisplay>& pickerDisplays);
    void drainDisplayFaceScans();
    void consumeDisplayFaceScan(DisplayFaceScan& scan);
    void logPickerSuggestions(const std::vector<PickerDisplay>& pickerDisplays);
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

    std::vector<WindowCandidate> m_windowCandidates;
    std::vector<FaceCandidate> m_faceCandidates;
    /// The global region's border label: the captured display's name,
    /// refreshed when the captured display changes.
    std::string m_displayLabel;
    uint32_t m_displayLabelId = 0;

    /// The face-probe mailbox: a detached detection thread fills it, the
    /// main loop drains it; at most one probe is in flight.
    struct FaceLockProbe
    {
        std::atomic<bool> running{false};
        std::atomic<bool> ready{false};
        std::mutex mutex;
        std::vector<IntRect> faces;  ///< detector boxes, full-frame pixels
        uint64_t forWindowIdentity = 0;
        IntRect roi;  ///< the searched area, for judging edge-clipped boxes
    };

    std::map<uint64_t, AppFaceLock> m_faceLocks;
    FaceLockProbe m_faceLockProbe;

    /// One non-streamed display's background face scan for an open picker: a
    /// detached thread grabs that display off the capture stream, detects,
    /// and fills this; the main loop drains it and pushes the boxes into the
    /// picker overlay. Held by unique_ptr so the detached thread's raw
    /// pointer stays valid across the vector's growth, and never erased while
    /// @c running.
    struct DisplayFaceScan
    {
        std::atomic<bool> running{false};
        std::atomic<bool> ready{false};
        std::mutex mutex;
        uint32_t displayId = 0;
        uint64_t generation = 0;     ///< which picker opening spawned it
        std::vector<IntRect> faces;  ///< detector boxes, guarded by mutex
        int frameWidth = 0;          ///< the grabbed frame's size, guarded by mutex
        int frameHeight = 0;
        double elapsedMs = 0.0;  ///< grab plus detect, for the diagnostics line
    };

    std::vector<std::unique_ptr<DisplayFaceScan>> m_displayFaceScans;
    /// Bumped every time the picker opens, stamped on each scan it spawns: a
    /// scan that lands after its picker closed or a newer one opened is
    /// dropped rather than fed to the wrong overlay.
    uint64_t m_facePickGeneration = 0;
    std::array<IconTexture, IconCount> m_iconTextures;
    double m_nextFaceLockProbe = 0.0;
    /// True while the active lock's face is not confirmed where the region
    /// sits; the border hides instead of outlining stale content.
    bool m_faceLockHunting = false;
    /// The content-stability probe over the face-locked region: a sparse pixel
    /// grid compared frame to frame; the border shows only settled content.
    std::vector<uint8_t> m_regionContentSamples;
    RegionOfInterest m_regionContentRect;
    double m_regionContentChangedAt = -1.0;
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

    bool m_regionPicking = false;
    bool m_regionPickIsPin = false;
    bool m_regionPickSwallowCancel = false;

    MarkerSmoother m_vectorscopeMarker;
    MarkerSmoother m_waveformMarker;

    std::array<LayoutPreset, LayoutPresetSlots> m_layoutPresets;
    // The last loaded or saved preset slot, 1-9; 0 when none is active. The
    // toolbar badge names it, starred when the live layout has drifted.
    int m_activePresetSlot = 0;
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
    std::optional<RegionPickerMode> m_wantRegionPick;
    std::optional<AnalysisWorker::FrameSize> m_frameSize;
    std::vector<ImVec4> m_paneRects;
};

}  // namespace sidescopes
