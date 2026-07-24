#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "app/attach_controller.h"
#include "app/face_lock.h"
#include "core/analysis_worker.h"
#include "core/frame.h"

namespace sidescopes {

class CaptureController;

/// What one FaceLockController step decided that the host must apply. At most
/// one field is set: the controller owns the lock state and mutates it in
/// place, but it never touches the analysis region or the attachment itself -
/// it returns the intent and the host carries it out.
struct FaceLockOutcome
{
    /// An adopted anchor's region: the host makes it the analysis region.
    std::optional<RegionOfInterest> applyRegion;
    /// A lock that gave up: the host removes the attachment and returns to the
    /// global region. Carries the identity that was lost.
    std::optional<uint64_t> lostLock;
};

/// Owns the face locks, the background detection probe, and the content
/// stability watch that together follow a face inside an attached window. The
/// pure adopt/hold/give-up judgement lives in face_lock::decide; this drives
/// it: it prunes locks whose windows are gone, launches the detached probe,
/// consumes its result, runs the content probe, and returns a FaceLockOutcome
/// the host applies. It reads the attach, worker, and capture controllers it
/// is constructed with, and calls the platform desktop and detection seams
/// directly, so a probe result can be judged end to end through
/// ingestProbeResult without a detection thread.
class FaceLockController
{
public:
    /// @p attach maps adopted regions and answers which windows are attached;
    /// @p worker supplies the latest frame the probe copies and the content
    /// watch samples; @p capture names the display the geometry is read
    /// against. All three must outlive the controller.
    FaceLockController(AttachController& attach, AnalysisWorker& worker, CaptureController& capture);

    /// Records a lock from a confirmed face pick: @p state is the pure lock the
    /// pick built, @p verifiedAt the clock the anchor was last confirmed at,
    /// and @p windowRect the window rectangle the pick saw, so the first
    /// same-size move carries the anchor.
    void addLock(uint64_t identity, FaceLockState state, double verifiedAt,
                 std::optional<AttachWindowRect> windowRect = std::nullopt);

    /// Drops the lock on @p identity without any outcome: a manual pick or
    /// draw replacing whatever lock the window wore.
    void removeLock(uint64_t identity);

    /// Forgets every lock and the hunting and content state - Watch Full
    /// Screen and an explicit detach.
    void clear();

    /// Activating a locked window whose anchor is older than a probe period
    /// starts a hunt and a probe at once, so the border does not flash a stale
    /// region while the first verdict is in flight. @p now is the frame clock.
    void onActivated(uint64_t identity, double now);

    /// A border edit while locked re-teaches the crop: @p region becomes the
    /// rectangle the face carries from here on, in @p frameSize pixels. A no-op
    /// when @p identity is not locked.
    void rebindCrop(uint64_t identity, const RegionOfInterest& region, AnalysisWorker::FrameSize frameSize);

    /// One per-frame step: prunes closed locks, consumes a finished probe,
    /// carries the active lock with its window, runs the content watch, and
    /// launches the next probe when due. @p decision is this frame's attach
    /// verdict, @p activeWindowIdentity the window whose geometry an adopted
    /// region is mapped through, @p analysisRegion the region the content watch
    /// samples, @p gestureActive whether a drag or move is in progress (the
    /// probe sits those out), and @p now the frame clock.
    [[nodiscard]] FaceLockOutcome update(const AttachDecision& decision,
                                         std::optional<AnalysisWorker::FrameSize> frameSize,
                                         uint64_t activeWindowIdentity, const RegionOfInterest& analysisRegion,
                                         bool gestureActive, double now);

    /// The probe consume/decide seam, injectable without a detection thread:
    /// filters @p boxes to trustworthy candidates against @p roi, lets
    /// face_lock::decide judge them, and returns the outcome. @p forIdentity is
    /// the window the probe searched; a result whose window is no longer the
    /// decision's active window is ignored. On give-up the lock is erased and
    /// @c lostLock is set; on adoption @c applyRegion carries the mapped region.
    [[nodiscard]] FaceLockOutcome ingestProbeResult(const std::vector<IntRect>& boxes, IntRect roi,
                                                    uint64_t forIdentity, const AttachDecision& decision,
                                                    uint64_t activeWindowIdentity,
                                                    std::optional<AnalysisWorker::FrameSize> frameSize, double now);

    /// Samples @p region of the latest frame and flags a content change when it
    /// differs enough from the previous sample. The idle wait drives this
    /// directly so a pan under an idle loop hides the border within a slice.
    void probeContentChange(const RegionOfInterest& region, std::optional<AnalysisWorker::FrameSize> frameSize,
                            double now);

    /// @return Whether @p identity currently holds a face lock.
    [[nodiscard]] bool contains(uint64_t identity) const;

    /// @return Whether the active lock's face is not confirmed where the region
    ///         sits; the border hides instead of outlining stale content.
    [[nodiscard]] bool hunting() const;

    /// @return Whether the locked region's content changed within the settle
    ///         time of @p now and the border should stay hidden.
    [[nodiscard]] bool contentUnsettled(double now) const;

    /// @return Whether a detection probe is in flight; the shutdown drain waits
    ///         on this before the detached thread's target leaves scope.
    [[nodiscard]] bool probeRunning() const;

private:
    /// A lock plus the window rectangle it last saw: a same-size window move
    /// translates the lock's anchors along with the face. The verdict stamp
    /// says when a probe (or the creating pick) last confirmed the anchor; an
    /// anchor older than a probe period is stale on activation and must not
    /// dress the border until re-verified.
    struct Lock
    {
        FaceLockState state;
        std::optional<AttachWindowRect> windowRect;
        double anchorVerifiedAt = 0.0;
    };

    /// The face-probe mailbox: a detached detection thread fills it, the main
    /// loop drains it; at most one probe is in flight.
    struct Probe
    {
        std::atomic<bool> running{false};
        std::atomic<bool> ready{false};
        std::mutex mutex;
        std::vector<IntRect> faces;  ///< detector boxes, full-frame pixels
        uint64_t forWindowIdentity = 0;
        IntRect roi;  ///< the searched rectangle, for judging edge-clipped boxes
    };

    [[nodiscard]] static std::vector<uint8_t> sampleContentGrid(const FrameView& view, const RegionOfInterest& region);
    void carryLockWithWindow(Lock& lock, const AttachWindowRect& rect,
                             std::optional<AnalysisWorker::FrameSize> frameSize);
    void launchProbe(const AttachDecision& decision, const FaceLockState& lock);
    [[nodiscard]] std::optional<RegionOfInterest> mappedRegion(const FaceLockState& lock,
                                                               std::optional<AnalysisWorker::FrameSize> frameSize,
                                                               uint64_t activeWindowIdentity);

    AttachController& m_attach;
    AnalysisWorker& m_worker;
    CaptureController& m_capture;

    std::map<uint64_t, Lock> m_locks;
    Probe m_probe;
    double m_nextProbe = 0.0;
    bool m_hunting = false;

    // The content-stability probe over the face-locked region: a sparse pixel
    // grid compared frame to frame; the border shows only settled content.
    std::vector<uint8_t> m_contentSamples;
    RegionOfInterest m_contentRect;
    double m_contentChangedAt = -1.0;
};

}  // namespace sidescopes
