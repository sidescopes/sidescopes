#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "app/face_tracking.h"
#include "core/analysis_worker.h"
#include "platform/face_detection.h"

namespace sidescopes {

/// Main-thread selection snapshot. Geometry is in display pixels, and a new
/// revision invalidates any work already in flight for the previous snapshot.
struct FaceTrackingCommand
{
    uint64_t revision = 0;
    uint64_t lockGeneration = 0;
    uint64_t identity = 0;
    uint64_t captureEpoch = 0;
    uint64_t captureContinuity = 0;
    uint32_t displayId = 0;
    int displayWidth = 0;
    int displayHeight = 0;
    IntRect window;
    FaceLockState crop;
    double minimumFacePixels = 36.0;
    bool enabled = false;
    std::shared_ptr<const std::map<uint64_t, uint64_t>> activeLocks;
};

struct FaceTrackingUpdate
{
    uint64_t serial = 0;
    uint64_t revision = 0;
    uint64_t identity = 0;
    uint64_t frameSequence = 0;
    FrameStamp stamp;
    face_tracking::Decision decision;
    RegionOfInterest region;
};

/// Two latest-value slots; neither side holds a lock during native detection,
/// analysis or notification. No queued frames or references to UI state cross
/// this boundary.
class FaceTrackingExchange
{
public:
    explicit FaceTrackingExchange(std::function<void()> notify);
    void select(const FaceTrackingCommand& command);
    [[nodiscard]] FaceTrackingCommand selection() const;
    bool publish(FaceTrackingUpdate update);
    [[nodiscard]] bool matches(const FaceTrackingCommand& command) const;
    [[nodiscard]] std::optional<FaceTrackingUpdate> fetch(uint64_t& lastSeen) const;

private:
    mutable std::mutex m_mutex;
    FaceTrackingCommand m_command;
    std::optional<FaceTrackingUpdate> m_update;
    uint64_t m_serial = 0;
    std::function<void()> m_notify;
};

using FaceSessionFactory = std::function<std::unique_ptr<FaceDetectionSession>()>;

/// Resolves one face region on the analysis thread before that frame's scope
/// pass. Native model and policy share its lifetime and execution owner.
class FaceTrackingWorker
{
public:
    FaceTrackingWorker(std::shared_ptr<FaceTrackingExchange> exchange, FaceSessionFactory factory,
                       std::function<double()> clock = frameClockSeconds);
    [[nodiscard]] FrameRegionResolution resolve(const FrameRegionRequest& request);

private:
    struct SearchArea
    {
        IntRect rect;
        double faceWidth = 0.0;
    };

    struct PolicyState
    {
        FaceTrackingCommand command;
        std::optional<face_tracking::Association> policy;
        std::optional<SearchArea> search;
    };

    void prunePolicies(const FaceTrackingCommand& command);
    [[nodiscard]] static bool remapPolicy(const FaceTrackingCommand& command, PolicyState& state);
    [[nodiscard]] FrameRegionResolution publishResolution(const FrameRegionRequest& request,
                                                          const FaceTrackingCommand& command,
                                                          const face_tracking::Decision& decision);
    [[nodiscard]] static bool preparePolicy(const FaceTrackingCommand& command, PolicyState& state);
    [[nodiscard]] FaceDetectionResult runDetector(const FrameView& crop, double minimumPixels);
    [[nodiscard]] face_tracking::Decision detect(const FrameView& frame, const FaceTrackingCommand& command,
                                                 PolicyState& state);
    [[nodiscard]] static bool matchesSource(const FrameView& frame, const FaceTrackingCommand& command);

    std::shared_ptr<FaceTrackingExchange> m_exchange;
    FaceSessionFactory m_factory;
    std::unique_ptr<FaceDetectionSession> m_detector;

    std::map<uint64_t, PolicyState> m_policies;
    std::shared_ptr<const std::map<uint64_t, uint64_t>> m_activeLocks;
    std::function<double()> m_clock;
    bool m_factoryAttempted = false;
};

}  // namespace sidescopes
