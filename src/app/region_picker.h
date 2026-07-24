#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/attach_controller.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/region_suggestions.h"
#include "platform/region_selection.h"

namespace sidescopes {

class CaptureController;
class ScreenCaptureSource;

/// A region the user confirmed in the picker, distilled for the host to apply:
/// the region in its display's percentages, which display it belongs to, and
/// whether it was confirmed in a window-attach mode. The overlays switch modes
/// on their own keys, so only the picker knows whether a rectangle confirmed
/// right now may attach to the window under it.
struct ConfirmedPick
{
    RegionOfInterest region;
    uint32_t displayId = 0;
    bool attachesToWindow = false;
};

/// What one RegionPicker step decided that the host must apply. The picker owns
/// its own state; it never writes the analysis region, the pin board, the
/// attachment, or the region border - it returns the intent and the host
/// carries it out. Several fields can be set at once (a preview that also ends
/// the pick), so each is applied in turn.
struct RegionPickOutcome
{
    /// A live preview: the host makes it the analysis region at once, its own
    /// no-op check deciding whether anything actually moved.
    std::optional<RegionOfInterest> previewRegion;
    /// A colour the pin tool sampled: the host adds it to the pin board.
    std::optional<FloatColor> pinColor;
    /// A confirmed region pick: the host attaches or draws it.
    std::optional<ConfirmedPick> confirmed;
    /// Cancelled with Esc, and not by a tool switch: the host resets to full
    /// screen. A cancel ordered by a tool switch is not the user's Esc and
    /// resets nothing.
    bool cancelled = false;
    /// The pick ended (confirm or cancel): the host re-syncs the region border.
    bool ended = false;
    /// Interaction happened worth marking: the host stamps its activity clock.
    bool activity = false;
};

/// Owns the region-picker lifecycle: opening the screenshot-style overlay, the
/// background per-display face scans, and the confirm/pin/preview polling. The
/// host coordination a pick resolves to - attaching a window, drawing the
/// global region, pinning a colour, resetting to full screen - stays with the
/// host, fed by the RegionPickOutcome this returns. It reads the capture
/// controller, worker, and capture source it is constructed with, and drives
/// the platform picker seams directly, so a poll can be judged end to end
/// through processPoll without the platform overlay.
class RegionPicker
{
public:
    /// What the picker keeps for one window it suggested, so a confirmed pick
    /// resolves back to its source: the rectangle the match runs against, and
    /// the identity the attachment is made on.
    struct WindowCandidate
    {
        uint64_t identity = 0;
        int64_t ownerPid = 0;
        std::string application;
        AttachWindowRect windowRect;
        RegionOfInterest region;
        uint32_t displayId = 0;
    };

    /// @p capture names the streamed display and reports capture liveness;
    /// @p worker supplies the frames the preview samples and the pin reads;
    /// @p source lists the displays the picker spans. All three must outlive
    /// the picker.
    RegionPicker(CaptureController& capture, AnalysisWorker& worker, ScreenCaptureSource& source);

    /// Requests a pick in @p mode, honoured on the next openIfRequested: the
    /// toolbar buttons, the keys, and the region menu set this.
    void request(RegionPickerMode mode);

    /// Clears any pending request. The frame loop calls this at the top of each
    /// frame, so a request lives only within the frame that raised it.
    void clearRequest();

    /// Opens the picker for a pending request, switches an open picker's mode,
    /// or closes it across the pin-versus-region tool boundary.
    /// @p regionIsFullScreen says whether the region the scopes read already
    /// covers the display, so a border-free frame is awaited only when a border
    /// is on screen. @return activity when a picker opened.
    [[nodiscard]] RegionPickOutcome openIfRequested(bool regionIsFullScreen);

    /// One poll of an open picker: reads the platform poll and processes it, or
    /// returns an empty outcome when no pick is active. @p frameSize is the
    /// streamed frame's size and @p screenSampleColor the throttled
    /// cross-display cursor sample, both for the pin tool.
    [[nodiscard]] RegionPickOutcome poll(std::optional<AnalysisWorker::FrameSize> frameSize,
                                         std::optional<FloatColor> screenSampleColor);

    /// The poll-to-outcome seam, injectable without the platform overlay:
    /// processes @p poll exactly as poll() does the poll it reads itself.
    [[nodiscard]] RegionPickOutcome processPoll(const RegionPickPoll& poll,
                                                std::optional<AnalysisWorker::FrameSize> frameSize,
                                                std::optional<FloatColor> screenSampleColor);

    /// Drains every finished background display scan into the open overlay.
    void drainFaceScans();

    /// @return Whether a pick is active.
    [[nodiscard]] bool active() const;

    /// @return Whether a background display scan's detached thread is still
    ///         running; the shutdown drain waits on this before the threads'
    ///         targets leave scope.
    [[nodiscard]] bool scansRunning() const;

    /// Recovers which window candidate a confirmed region names. The picker
    /// passes a window's exact rectangle through unchanged, so the match is a
    /// near-exact rectangle comparison; a freehand draw matches nothing.
    [[nodiscard]] const WindowCandidate* matchWindowCandidate(uint32_t displayId, const RegionOfInterest& region) const;

    /// The frontmost suggested window under a region's centre: the window a
    /// face pick or an in-attach-mode draw attaches to.
    [[nodiscard]] const WindowCandidate* windowContaining(uint32_t displayId, const RegionOfInterest& region) const;

    /// Recovers which face candidate a confirmed region names, by the same
    /// near-exact comparison the window match uses.
    [[nodiscard]] const FaceCandidate* matchFaceCandidate(uint32_t displayId, const RegionOfInterest& region) const;

private:
    /// One non-streamed display's background face scan for an open picker: a
    /// detached thread grabs that display off the capture stream, detects, and
    /// fills this; the main loop drains it and pushes the boxes into the picker
    /// overlay. Held by unique_ptr so the detached thread's raw pointer stays
    /// valid across the vector's growth, and never erased while @c running.
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

    void openRegionPicker(RegionPickerMode mode, bool regionIsFullScreen);
    void waitForBorderFreeFrame();
    [[nodiscard]] std::vector<PickerDisplay> buildPickerDisplays();
    [[nodiscard]] std::vector<SuggestedRegion> scanStreamedDisplayFaces(const FrameView& view, uint32_t streamed);
    void launchDisplayFaceScans(const std::vector<PickerDisplay>& pickerDisplays);
    void consumeDisplayFaceScan(DisplayFaceScan& scan);
    static void logPickerSuggestions(const std::vector<PickerDisplay>& pickerDisplays);
    [[nodiscard]] RegionPickOutcome processPinPoll(const RegionPickPoll& poll,
                                                   std::optional<AnalysisWorker::FrameSize> frameSize,
                                                   std::optional<FloatColor> screenSampleColor);
    void applyPinnedColor(const RegionPickPoll& poll, std::optional<AnalysisWorker::FrameSize> frameSize,
                          std::optional<FloatColor> screenSampleColor, RegionPickOutcome& outcome);
    [[nodiscard]] RegionPickOutcome processRegionPoll(const RegionPickPoll& poll);
    [[nodiscard]] static std::optional<FloatColor> averageRegionColor(const FrameView& view,
                                                                      const RegionOfInterest& region);
    [[nodiscard]] std::optional<FloatColor> averageFrameColor(const RegionOfInterest& region) const;

    CaptureController& m_capture;
    AnalysisWorker& m_worker;
    ScreenCaptureSource& m_source;

    bool m_picking = false;
    bool m_isPin = false;
    bool m_swallowCancel = false;
    std::optional<RegionPickerMode> m_want;

    std::vector<WindowCandidate> m_windowCandidates;
    std::vector<FaceCandidate> m_faceCandidates;

    std::vector<std::unique_ptr<DisplayFaceScan>> m_displayFaceScans;
    /// Bumped every time the picker opens, stamped on each scan it spawns: a
    /// scan that lands after its picker closed or a newer one opened is dropped
    /// rather than fed to the wrong overlay.
    uint64_t m_facePickGeneration = 0;
};

}  // namespace sidescopes
