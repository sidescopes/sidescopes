#include "app/face_tracking.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace sidescopes::face_tracking {
namespace {

bool validRect(const LockRect& rect)
{
    return std::isfinite(rect.left) && std::isfinite(rect.top) && std::isfinite(rect.right) &&
           std::isfinite(rect.bottom) && rect.left >= 0.0 && rect.top >= 0.0 &&
           rect.right <= std::numeric_limits<int>::max() && rect.bottom <= std::numeric_limits<int>::max() &&
           rect.right > rect.left && rect.bottom > rect.top;
}

bool within(const LockRect& rect, const LockRect& bounds)
{
    return validRect(rect) && rect.left >= bounds.left && rect.top >= bounds.top && rect.right <= bounds.right &&
           rect.bottom <= bounds.bottom;
}

bool validContext(Context context)
{
    return context.generation != 0 && context.epoch != 0 && context.display != 0;
}

bool validParameters(const Parameters& p)
{
    const double positive[] = {p.maximumResultAge,        p.holdSeconds,
                               p.shortRecoverySeconds,    p.maximumSpeedWidthsPerSecond,
                               p.displacementSlackWidths, p.maximumDisplacementWidths,
                               p.predictionScoreWeight,   p.maximumLogScaleStep,
                               p.logScaleGrowthPerSecond, p.scaleScoreWeight,
                               p.minimumScoreMargin,      p.maximumWinnerScoreRatio,
                               p.crossingSeparationWidths};
    return std::all_of(std::begin(positive), std::end(positive),
                       [](double value) { return std::isfinite(value) && value > 0.0 && value <= 1000000.0; }) &&
           p.maximumWinnerScoreRatio < 1.0 && p.predictionScoreWeight < 1.0 &&
           p.shortRecoverySeconds <= p.holdSeconds && p.maximumResultAge < p.holdSeconds;
}

double distance(const FaceAnchor& a, const FaceAnchor& b)
{
    return std::hypot(a.centerX - b.centerX, a.centerY - b.centerY);
}

}  // namespace

FaceAnchor anchorOf(const LockRect& box)
{
    return {(box.left + box.right) / 2.0, (box.top + box.bottom) / 2.0, box.right - box.left};
}

Association::Association(Stamp selection, const LockRect& selectedBox, const LockRect& crop, LockRect bounds,
                         double observedSeconds, Parameters parameters)
    : parameters_(parameters),
      lastSeen_(selection),
      bounds_(bounds),
      crop_(crop),
      acceptedSourceSeconds_(selection.sourceSeconds),
      observedSeconds_(observedSeconds),
      sourceTimeKnown_(true)
{
    if (!validContext(selection.context) || selection.sequence == 0 || selection.sourceSeconds < 0.0 ||
        !validParameters(parameters) || !validRect(bounds) || !within(selectedBox, bounds) ||
        selectedBox.right - selectedBox.left < 1.0 || !within(crop, bounds) ||
        !std::isfinite(selection.sourceSeconds) || !std::isfinite(observedSeconds) ||
        observedSeconds < selection.sourceSeconds ||
        observedSeconds - selection.sourceSeconds > parameters.maximumResultAge) {
        throw std::invalid_argument("Invalid or stale explicit face nomination");
    }
    cropState_ = sidescopes::face_lock::makeLock(anchorOf(selectedBox), crop);
}

bool validSelection(Context context, const FaceLockState& crop, LockRect bounds)
{
    const auto mapped = face_lock::mapRegion(crop, crop.lastAnchor);
    return !(!validContext(context) || !validRect(bounds) || !within(mapped, bounds) ||
             !std::isfinite(crop.lastAnchor.width) || crop.lastAnchor.width < 1.0 ||
             crop.lastAnchor.width > bounds.right - bounds.left || !std::isfinite(crop.lastAnchor.centerX) ||
             !std::isfinite(crop.lastAnchor.centerY) || crop.lastAnchor.centerX < bounds.left ||
             crop.lastAnchor.centerX > bounds.right || crop.lastAnchor.centerY < bounds.top ||
             crop.lastAnchor.centerY > bounds.bottom);
}

Association::Association(Context context, const FaceLockState& crop, LockRect bounds)
    : lastSeen_{context, 0, 0.0},
      cropState_(crop),
      bounds_(bounds),
      crop_(sidescopes::face_lock::mapRegion(crop, crop.lastAnchor)),
      nominationPending_(true)
{
    if (!validSelection(context, crop, bounds)) {
        throw std::invalid_argument("Invalid selected face crop");
    }
}

Decision Association::result(Action action, Reason reason, std::optional<std::size_t> selected) const
{
    return {action, reason, crop_, cropState_.lastAnchor, selected, acceptedSourceSeconds_, following_};
}

Decision Association::current() const
{
    return result(following_ ? Action::Held : Action::OrdinaryAttached,
                  following_ ? (uncertainSince_ ? uncertainReason_ : Reason::Waiting) : Reason::AlreadyAttached);
}

bool Association::expired(double observedSeconds) const
{
    return uncertainSince_ && observedSeconds - *uncertainSince_ >= parameters_.holdSeconds;
}

Decision Association::uncertain(Reason reason, double observedSeconds)
{
    uncertainReason_ = reason;
    if (!uncertainSince_) {
        uncertainSince_ = observedSeconds;
    }
    if (expired(observedSeconds)) {
        following_ = false;
        return result(Action::OrdinaryAttached, Reason::EvidenceExpired);
    }
    return result(Action::Held, reason);
}

std::optional<Decision> Association::beginObservation(Stamp stamp, double observedSeconds)
{
    if (!(stamp.context == lastSeen_.context)) {
        return result(Action::Ignored, Reason::StaleContext);
    }
    if (!following_) {
        return result(Action::OrdinaryAttached, Reason::AlreadyAttached);
    }
    if (!std::isfinite(stamp.sourceSeconds) || !std::isfinite(observedSeconds) ||
        observedSeconds < stamp.sourceSeconds || observedSeconds < observedSeconds_) {
        return result(Action::Ignored, Reason::InvalidClock);
    }
    if (stamp.sequence <= lastSeen_.sequence) {
        return result(Action::Ignored, Reason::DuplicateOrOlderFrame);
    }
    if (stamp.sourceSeconds < 0.0 || (sourceTimeKnown_ && stamp.sourceSeconds <= lastSeen_.sourceSeconds)) {
        return result(Action::Ignored, Reason::NonIncreasingSourceTime);
    }
    lastSeen_ = stamp;
    sourceTimeKnown_ = true;
    observedSeconds_ = observedSeconds;
    if (expired(observedSeconds)) {
        return uncertain(Reason::EvidenceExpired, observedSeconds);
    }
    if (observedSeconds - stamp.sourceSeconds > parameters_.maximumResultAge) {
        return uncertain(Reason::ExpiredResult, observedSeconds);
    }
    return {};
}

std::optional<Decision> Association::checkDetection(DetectionStatus status, std::span<const LockRect> boxes,
                                                    double observedSeconds)
{
    if (status == DetectionStatus::Unsupported) {
        following_ = false;
        return result(Action::OrdinaryAttached, Reason::Unsupported);
    }
    if (status == DetectionStatus::Failed) {
        return uncertain(Reason::NativeFailure, observedSeconds);
    }
    if (status != DetectionStatus::Completed) {
        return uncertain(Reason::NativeStatusUnavailable, observedSeconds);
    }
    for (const auto& box : boxes) {
        if (!within(box, bounds_) || box.right - box.left < 1.0) {
            return uncertain(Reason::InvalidDetection, observedSeconds);
        }
    }
    if (boxes.empty()) {
        return uncertain(Reason::SuccessfulEmpty, observedSeconds);
    }
    // A geometric crossing cannot establish who emerged afterwards. Retire
    // after the hold; never erase this latch because one box is left again.
    if (ambiguous_) {
        return uncertain(Reason::Ambiguous, observedSeconds);
    }
    if (uncertainSince_ && observedSeconds - *uncertainSince_ > parameters_.shortRecoverySeconds) {
        return uncertain(Reason::RecoveryUncertain, observedSeconds);
    }
    return {};
}

Association::Ranking Association::rank(std::span<const LockRect> boxes, double dt) const
{
    const auto& previous = cropState_.lastAnchor;
    const FaceAnchor predicted{previous.centerX + velocityX_ * dt, previous.centerY + velocityY_ * dt, previous.width};
    const double displacementGate =
        std::min(parameters_.maximumDisplacementWidths,
                 parameters_.displacementSlackWidths + parameters_.maximumSpeedWidthsPerSecond * dt);
    Ranking ranking{{}, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    for (std::size_t index = 0; index < boxes.size(); ++index) {
        const auto candidate = anchorOf(boxes[index]);
        const double logScale = std::abs(std::log(candidate.width / previous.width));
        const double residual = distance(candidate, predicted) / previous.width;
        const double displacement = distance(candidate, previous) / previous.width;
        if (displacement > displacementGate ||
            logScale > parameters_.maximumLogScaleStep + parameters_.logScaleGrowthPerSecond * dt) {
            continue;
        }
        // Prediction is an uncertain guide, never an eligibility gate. Raw
        // detector noise, stopping and reversal can make extrapolation wrong
        // while a box remains close to the last accepted face. Equal prior
        // and prediction weights leave the two endpoints equally plausible:
        // an old-position rival versus a predicted-position rival must not
        // choose identity solely by assuming either stopping or continuation.
        const double score = (1.0 - parameters_.predictionScoreWeight) * displacement +
                             parameters_.predictionScoreWeight * residual + parameters_.scaleScoreWeight * logScale;
        if (score < ranking.best) {
            ranking.second = ranking.best;
            ranking.best = score;
            ranking.winner = index;
        } else if (score < ranking.second) {
            ranking.second = score;
        }
    }
    return ranking;
}

bool Association::hasRival(std::span<const LockRect> boxes, const Ranking& ranking, std::size_t winner) const
{
    const auto candidate = anchorOf(boxes[winner]);
    const auto& previous = cropState_.lastAnchor;
    bool closeRival = false;
    // Include even a just-outside-gate rival: the position/scale gate must
    // not conceal a visible crossing. Large distant faces are not winners.
    for (std::size_t index = 0; index < boxes.size(); ++index) {
        if (index != winner &&
            distance(anchorOf(boxes[index]), candidate) / previous.width < parameters_.crossingSeparationWidths) {
            closeRival = true;
        }
    }
    return closeRival ||
           (std::isfinite(ranking.second) && (ranking.second - ranking.best < parameters_.minimumScoreMargin ||
                                              ranking.best > parameters_.maximumWinnerScoreRatio * ranking.second));
}

Decision Association::accept(Stamp stamp, const LockRect& box, std::size_t index, double dt)
{
    const auto candidate = anchorOf(box);
    const auto previous = cropState_.lastAnchor;
    const auto mapped = sidescopes::face_lock::mapRegion(cropState_, candidate);
    if (!within(mapped, bounds_)) {
        return uncertain(Reason::CropOutsideBounds, observedSeconds_);
    }
    // Only the search prediction uses velocity. Output is the raw current
    // detection, with no dead zone, settling requirement or interpolation.
    const double dx = candidate.centerX - previous.centerX;
    const double dy = candidate.centerY - previous.centerY;
    const double movement = std::hypot(dx, dy);
    const double maximumSpeed = parameters_.maximumSpeedWidthsPerSecond * candidate.width;
    if (movement > maximumSpeed * dt) {
        velocityX_ = dx / movement * maximumSpeed;
        velocityY_ = dy / movement * maximumSpeed;
    } else {
        velocityX_ = dx / dt;
        velocityY_ = dy / dt;
    }
    cropState_.lastAnchor = candidate;
    crop_ = mapped;
    acceptedSourceSeconds_ = stamp.sourceSeconds;
    nominationPending_ = false;
    uncertainSince_.reset();
    return result(Action::Accepted, Reason::Followed, index);
}

Decision Association::advance(Stamp stamp, double observedSeconds, DetectionStatus status,
                              std::span<const LockRect> boxes)
{
    if (const auto rejected = beginObservation(stamp, observedSeconds)) {
        return *rejected;
    }
    if (const auto rejected = checkDetection(status, boxes, observedSeconds)) {
        return *rejected;
    }
    // Withheld unchanged frames do not widen the jump gate on a still photo.
    const double dt = nominationPending_
                          ? parameters_.shortRecoverySeconds
                          : std::min(stamp.sourceSeconds - acceptedSourceSeconds_, parameters_.shortRecoverySeconds);
    const Ranking ranking = rank(boxes, dt);
    if (!ranking.winner) {
        return uncertain(Reason::NoPlausibleMatch, observedSeconds);
    }
    if (hasRival(boxes, ranking, *ranking.winner)) {
        ambiguous_ = true;
        return uncertain(Reason::Ambiguous, observedSeconds);
    }
    return accept(stamp, boxes[*ranking.winner], *ranking.winner, dt);
}

Decision Association::tick(Context context, double observedSeconds)
{
    if (!(context == lastSeen_.context)) {
        return result(Action::Ignored, Reason::StaleContext);
    }
    if (!following_) {
        return result(Action::OrdinaryAttached, Reason::AlreadyAttached);
    }
    if (!std::isfinite(observedSeconds) || observedSeconds < observedSeconds_) {
        return result(Action::Ignored, Reason::InvalidClock);
    }
    observedSeconds_ = observedSeconds;
    if (expired(observedSeconds)) {
        return uncertain(Reason::EvidenceExpired, observedSeconds);
    }
    return result(Action::Held, uncertainSince_ ? uncertainReason_ : Reason::Waiting);
}

Decision Association::rebindCrop(Context nextContext, const LockRect& crop)
{
    if (nextContext.generation <= lastSeen_.context.generation || nextContext.epoch != lastSeen_.context.epoch ||
        nextContext.display != lastSeen_.context.display || !within(crop, bounds_)) {
        throw std::invalid_argument("Crop edit needs a newer generation in the same geometry/source");
    }
    lastSeen_.context = nextContext;
    sidescopes::face_lock::rebindCrop(cropState_, crop);
    crop_ = crop;
    // Editing the crop does not verify a face, erase ambiguity, or restart a
    // retired follower. Retain sequence and evidence time to reject late work.
    return result(following_ ? Action::Held : Action::OrdinaryAttached, Reason::ManualCrop);
}

Decision Association::remap(Context nextContext, const FaceLockState& crop, LockRect bounds, bool resumedStream)
{
    if (nextContext.generation <= lastSeen_.context.generation ||
        (!resumedStream && nextContext.epoch != lastSeen_.context.epoch) ||
        (resumedStream && nextContext.epoch <= lastSeen_.context.epoch) ||
        nextContext.display != lastSeen_.context.display || !validSelection(nextContext, crop, bounds)) {
        return result(Action::Ignored, Reason::InvalidDetection);
    }
    cropState_ = crop;
    bounds_ = bounds;
    crop_ = face_lock::mapRegion(crop, crop.lastAnchor);
    lastSeen_.context = nextContext;
    if (resumedStream) {
        lastSeen_.sequence = 0;
    }
    return result(following_ ? Action::Held : Action::OrdinaryAttached, Reason::ManualCrop);
}

Decision Association::retire(Context context, const FaceLockState& crop, LockRect bounds)
{
    if (!validSelection(context, crop, bounds)) {
        return result(Action::Ignored, Reason::InvalidDetection);
    }
    lastSeen_.context = context;
    cropState_ = crop;
    bounds_ = bounds;
    crop_ = face_lock::mapRegion(crop, crop.lastAnchor);
    following_ = false;
    return result(Action::OrdinaryAttached, Reason::StaleContext);
}

std::string_view name(Action value)
{
    switch (value) {
    case Action::Accepted:
        return "accepted";
    case Action::Held:
        return "held";
    case Action::OrdinaryAttached:
        return "ordinary_attached";
    case Action::Ignored:
        return "ignored";
    }
    return "invalid_action";
}

std::string_view name(Reason value)
{
    static constexpr std::string_view Names[] = {
        "selected",
        "followed",
        "successful_empty",
        "no_plausible_match",
        "ambiguous",
        "recovery_uncertain",
        "native_failure",
        "native_status_unavailable",
        "unsupported",
        "invalid_detection",
        "crop_outside_bounds",
        "stale_context",
        "duplicate_or_older_frame",
        "nonincreasing_source_time",
        "invalid_clock",
        "expired_result",
        "evidence_expired",
        "already_attached",
        "manual_crop",
        "waiting",
    };
    const auto index = static_cast<std::size_t>(value);
    return index < std::size(Names) ? Names[index] : "invalid_reason";
}

}  // namespace sidescopes::face_tracking
