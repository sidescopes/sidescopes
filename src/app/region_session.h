#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/attach_controller.h"
#include "app/face_lock_controller.h"
#include "app/region_coordinator.h"
#include "app/region_picker.h"
#include "platform/desktop.h"

namespace sidescopes {

/// Changes the application shell must publish after one region operation.
/// An empty region is meaningful only when regionChanged is set.
struct RegionSessionOutcome
{
    bool regionChanged = false;
    std::optional<RegionOfInterest> region;
    std::optional<FloatColor> pinColor;
    std::optional<std::string> status;
    bool activity = false;
};

/// Owns the selected region's lifecycle: window attachment and focus routing,
/// face tracking, picker confirmation, border editing and motion observation.
/// The shell publishes the resulting selection to analysis and owns the UI
/// status and pin board. Capture and worker dependencies must outlive this.
class RegionSession
{
public:
    RegionSession(CaptureController& capture, AnalysisWorker& worker, ScreenCaptureSource& source);
    ~RegionSession();
    RegionSession(const RegionSession&) = delete;
    RegionSession& operator=(const RegionSession&) = delete;

    /// Stops native callbacks and drains outstanding detection before the
    /// platform event loop and the session's collaborators are destroyed.
    void shutdown();

    [[nodiscard]] RegionPicker& picker();
    [[nodiscard]] const AttachController& attachments() const;
    [[nodiscard]] bool interacting() const;
    [[nodiscard]] bool carried() const;
    [[nodiscard]] bool faceLocked() const;
    [[nodiscard]] bool backgroundWorkRunning() const;

    [[nodiscard]] RegionSessionOutcome initializeGlobalRegion(const RegionOfInterest& region);
    [[nodiscard]] RegionSessionOutcome follow(bool windowMinimized, std::optional<AnalysisWorker::FrameSize> frameSize);
    [[nodiscard]] RegionSessionOutcome poll(bool windowMinimized, std::optional<AnalysisWorker::FrameSize> frameSize,
                                            std::optional<FloatColor> screenSampleColor);
    [[nodiscard]] RegionSessionOutcome clear();
    [[nodiscard]] RegionSessionOutcome dismiss();
    [[nodiscard]] RegionSessionOutcome detach();
    void syncBorder(bool windowMinimized);
    void idleWaitWatchingAttachedWindow();

private:
    [[nodiscard]] RegionSessionOutcome takeOutcome();
    void setStatus(std::string message);
    [[nodiscard]] RegionBorderState borderState() const;
    void applyRegionOutcome(const RegionOutcome& outcome);
    void followAttachedWindow();
    [[nodiscard]] std::vector<AttachedWindowObservation> gatherAttachedObservations() const;
    [[nodiscard]] bool activeWindowMoved(const AttachDecision& decision) const;
    void captureActiveDisplay(const AttachDecision& decision);
    void applyAttachDecision(const AttachDecision& decision);
    void refreshAttachedLabel(const AttachDecision& decision);
    [[nodiscard]] std::optional<uint64_t> resolveFocusedWindow() const;
    void onWindowMotion(WindowMotionSignal signal);
    void detachActiveWindow();
    void releaseActiveWindow();
    void confirmPickedRegion(const ConfirmedPick& pick);
    void adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region);
    void dismissEditedBorder();
    void toggleRegionBinding();
    void attachGlobalRegionToWindow();
    void applyBorderEdit(const RegionOfInterest& edited);
    void applyFaceLockOutcome(const FaceLockOutcome& outcome);
    bool adoptFacePick(uint32_t displayId, const RegionOfInterest& confirmed);
    static void logAttachMapping(const RegionPicker::WindowCandidate& picked, const RegionOfInterest& start);
    void applyRegionPickOutcome(const RegionPickOutcome& outcome);
    void applyBorderEditOutcome(const RegionBorderEditOutcome& outcome);

    CaptureController& m_capture;
    std::optional<RegionOfInterest> m_region;
    AttachController m_attach;
    FaceLockController m_faceLock;
    RegionPicker m_regionPicker;
    RegionCoordinator m_regions;
    int64_t m_ownPid;

    bool m_attachedWindowMoving = false;
    bool m_attachGripActive = false;
    double m_attachRegionMovedAt = -1.0;
    uint64_t m_activeWindowIdentity = 0;
    std::optional<AttachWindowRect> m_attachLastSeenRect;
    std::string m_attachActiveLabel;
    bool m_windowMinimized = false;
    std::optional<AnalysisWorker::FrameSize> m_frameSize;
    RegionSessionOutcome m_pending;
    bool m_stopped = false;
};

}  // namespace sidescopes
