#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "app/face_lock.h"

namespace sidescopes::face_tracking {

using sidescopes::FaceAnchor;
using sidescopes::LockRect;

struct Context
{
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    std::uint32_t display = 0;
    bool operator==(const Context&) const = default;
};

struct Stamp
{
    Context context;
    std::uint64_t sequence = 0;
    double sourceSeconds = 0.0;
};

enum class DetectionStatus
{
    Completed,
    Failed,
    Unsupported,
    Unavailable
};
enum class Action
{
    Accepted,
    Held,
    OrdinaryAttached,
    Ignored,
    Searching
};
enum class Reason
{
    Selected,
    Followed,
    SuccessfulEmpty,
    NoPlausibleMatch,
    Ambiguous,
    RecoveryUncertain,
    NativeFailure,
    NativeStatusUnavailable,
    Unsupported,
    InvalidDetection,
    CropOutsideBounds,
    StaleContext,
    DuplicateOrOlderFrame,
    NonIncreasingSourceTime,
    InvalidClock,
    ExpiredResult,
    EvidenceExpired,
    AlreadyAttached,
    ManualCrop,
    Waiting
};

// Bounded geometric association: brief misses retain the crop, while a long
// absence hides readings while retaining the selection and looking for evidence.
struct Parameters
{
    double maximumResultAge = 0.20;
    double holdSeconds = 1.0;
    double rivalMemorySeconds = 0.40;
    double shortRecoverySeconds = 0.16;
    double maximumSpeedWidthsPerSecond = 6.0;
    double displacementSlackWidths = 0.10;
    double maximumDisplacementWidths = 1.10;
    double predictionScoreWeight = 0.50;
    double maximumLogScaleStep = 0.26236426446749106;  // log(1.3)
    double logScaleGrowthPerSecond = 1.0;
    double scaleScoreWeight = 0.35;
    double minimumScoreMargin = 0.20;
    double maximumWinnerScoreRatio = 0.60;
    double crossingSeparationWidths = 0.65;
};

struct Decision
{
    Action action = Action::Ignored;
    Reason reason = Reason::Waiting;
    LockRect crop;
    FaceAnchor anchor;
    std::optional<std::size_t> selectedBox;
    double evidenceSourceSeconds = 0.0;
    bool following = true;
    // One worker-owned deadline also governs capture-silent UI expiry.
    std::optional<double> uncertaintyDeadline;
    uint64_t readingGeneration = 1;
};

// Coordinates stay in one fixed display-pixel space for this context. The
// caller must invalidate/reseed on window transform, DPI or capture changes.
// A source stamp and observedSeconds use the same monotonic clock. Receipt
// time bounds queued work; it does not measure compositor presentation age.
class Association
{
public:
    /// A selected or mechanically translated anchor still needs a fresh
    /// detection before it is reported as followed. It supplies a geometric
    /// prior, without inventing a new observation timestamp.
    Association(Context context, const FaceLockState& crop, LockRect bounds);

    Association(Stamp selection, const LockRect& selectedBox, const LockRect& crop, LockRect bounds,
                double observedSeconds, Parameters parameters = {});

    [[nodiscard]] Decision advance(Stamp stamp, double observedSeconds, DetectionStatus status,
                                   std::span<const LockRect> boxes);
    // A clock tick may expire evidence; it never counts as a detection/miss.
    [[nodiscard]] Decision tick(Context context, double observedSeconds);
    // Only a crop edit in the same source/geometry; other context changes
    // require a fresh explicitly selected anchor, not a guessed identity.
    [[nodiscard]] Decision rebindCrop(Context nextContext, const LockRect& crop);
    [[nodiscard]] Decision current() const;
    /// A resumed stream requires controller-proven source continuity. It may
    /// reset physical frame numbering, but never clears loss or ambiguity.
    /// The saved normalized crop and anchor form one control snapshot; a newer
    /// unconsumed detection must not change their relationship during remapping.
    /// Parent translation is explicit because display clipping changes bounds.
    [[nodiscard]] Decision remap(Context nextContext, const FaceLockState& crop, LockRect bounds, double parentDx,
                                 double parentDy, bool resumedStream = false);
    [[nodiscard]] Decision retire(Context context, const FaceLockState& crop, LockRect bounds);

private:
    [[nodiscard]] Decision result(Action action, Reason reason, std::optional<std::size_t> selected = {}) const;
    [[nodiscard]] Decision uncertain(Reason reason, double observedSeconds);
    [[nodiscard]] bool expired(double observedSeconds) const;

    struct Ranking
    {
        std::optional<std::size_t> winner;
        double best;
        double second;
    };

    [[nodiscard]] std::optional<Decision> beginObservation(Stamp stamp, double observedSeconds);
    [[nodiscard]] std::optional<Decision> checkDetection(DetectionStatus status, std::span<const LockRect> boxes,
                                                         double observedSeconds);
    [[nodiscard]] Ranking rank(std::span<const LockRect> boxes, double dt) const;
    [[nodiscard]] bool hasRival(std::span<const LockRect> boxes, const Ranking& ranking, std::size_t winner) const;
    [[nodiscard]] Decision accept(Stamp stamp, const LockRect& box, std::size_t index, double dt);

    struct Rival
    {
        FaceAnchor anchor;
        double sourceSeconds;
    };

    void forgetOldRivals(double sourceSeconds);
    [[nodiscard]] bool matchesRecentRival(const FaceAnchor& candidate, double sourceSeconds) const;
    [[nodiscard]] bool rememberRival(const FaceAnchor& rival, double sourceSeconds);
    [[nodiscard]] bool rememberRivals(std::span<const LockRect> boxes, std::size_t winner, double sourceSeconds);
    void translateRivals(double dx, double dy);

    // Negative geometric evidence is bounded independently of detector output.
    // It never supplies a selected face or renews that face's observation time.
    static constexpr std::size_t MaximumRivals = 8;
    std::array<std::optional<Rival>, MaximumRivals> rivals_;
    Parameters parameters_;
    Stamp lastSeen_;
    sidescopes::FaceLockState cropState_;
    LockRect bounds_;
    LockRect crop_;
    double acceptedSourceSeconds_ = 0.0;
    double observedSeconds_ = 0.0;
    // Receipt time remains monotonic when a resumed stream resets numbering.
    bool sourceTimeKnown_ = false;
    std::optional<double> uncertainSince_;
    Reason uncertainReason_ = Reason::Waiting;
    double velocityX_ = 0.0;
    double velocityY_ = 0.0;
    bool ambiguous_ = false;
    bool following_ = true;
    bool nominationPending_ = false;
    uint64_t readingGeneration_ = 1;
};

[[nodiscard]] std::string_view name(Action value);
[[nodiscard]] std::string_view name(Reason value);
[[nodiscard]] FaceAnchor anchorOf(const LockRect& box);
[[nodiscard]] bool validSelection(Context context, const FaceLockState& crop, LockRect bounds);

}  // namespace sidescopes::face_tracking
