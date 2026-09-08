#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "app/attach_controller.h"
#include "app/face_lock.h"
#include "app/face_tracking_worker.h"
#include "core/analysis_worker.h"

namespace sidescopes {

class CaptureController;

struct FaceLockOutcome
{
    std::optional<RegionOfInterest> applyRegion;
    /// Face following ends; the ordinary window attachment retains its crop.
    std::optional<uint64_t> lostLock;
};

/// Main-thread owner of face selections and their window transforms. Native
/// detection and geometric association belong to the worker; only stamped
/// decisions cross back to update the attachment and border.
class FaceLockController
{
public:
    FaceLockController(AttachController& attach, AnalysisWorker& worker, CaptureController& capture,
                       std::function<double()> clock = frameClockSeconds);
    void addLock(uint64_t identity, FaceLockState state, std::optional<AttachWindowRect> windowRect = std::nullopt,
                 std::optional<std::pair<int, int>> coordinateSize = std::nullopt);
    void removeLock(uint64_t identity);
    void clear();
    void activationChanged();
    void rebindCrop(uint64_t identity, const RegionOfInterest& region, AnalysisWorker::FrameSize frameSize);

    /// User selection or source changes invalidate work before the next pass.
    void invalidate();
    [[nodiscard]] uint64_t selectionRevision() const;
    [[nodiscard]] FaceLockOutcome update(const AttachDecision& decision,
                                         std::optional<AnalysisWorker::FrameSize> frameSize, bool gestureActive,
                                         double now);
    [[nodiscard]] bool contains(uint64_t identity) const;
    [[nodiscard]] bool locked() const;

private:
    struct Lock
    {
        FaceLockState state;
        std::optional<AttachWindowRect> windowRect;
        uint64_t generation = 0;
        std::optional<double> uncertaintyDeadline;
        std::optional<std::pair<int, int>> coordinateSize;
    };

    void carryLockWithWindow(Lock& lock, const AttachWindowRect& rect,
                             std::optional<AnalysisWorker::FrameSize> frameSize);
    void refreshInventory();
    [[nodiscard]] FaceTrackingCommand makeCommand(const AttachDecision& decision,
                                                  std::optional<AnalysisWorker::FrameSize> frameSize,
                                                  bool gestureActive) const;
    [[nodiscard]] FaceLockOutcome consume(const AttachDecision& decision, double now);
    [[nodiscard]] std::optional<RegionOfInterest> acceptRegion(const FaceTrackingUpdate& update,
                                                               const AttachDecision& decision);

    AttachController& m_attach;
    CaptureController& m_capture;
    std::shared_ptr<FaceTrackingExchange> m_exchange;
    std::map<uint64_t, Lock> m_locks;
    const std::shared_ptr<const std::map<uint64_t, uint64_t>> m_emptyLocks =
        std::make_shared<const std::map<uint64_t, uint64_t>>();
    std::shared_ptr<const std::map<uint64_t, uint64_t>> m_activeLocks = m_emptyLocks;
    bool m_inventoryDirty = false;
    FaceTrackingCommand m_command;
    uint64_t m_revision = 0;
    uint64_t m_nextGeneration = 0;
    uint64_t m_lastSeen = 0;
};

}  // namespace sidescopes
