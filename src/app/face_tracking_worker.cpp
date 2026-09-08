#include "app/face_tracking_worker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/region_geometry.h"
#include "core/diagnostics.h"

namespace sidescopes {
namespace {

using face_tracking::Action;
using face_tracking::Context;
using face_tracking::DetectionStatus;
using face_tracking::Reason;

LockRect lockRect(IntRect rect)
{
    return {static_cast<double>(rect.x), static_cast<double>(rect.y), static_cast<double>(rect.x) + rect.width,
            static_cast<double>(rect.y) + rect.height};
}

Context contextOf(const FaceTrackingCommand& command)
{
    return {command.revision, command.captureEpoch, command.displayId};
}

bool sameCrop(const FaceLockState& x, const FaceLockState& y)
{
    // Automatic anchor feedback is not a new selection or manual crop edit.
    return x.offsetX == y.offsetX && x.offsetY == y.offsetY && x.sizeX == y.sizeX && x.sizeY == y.sizeY;
}

bool sameSourceGrid(const FaceTrackingCommand& a, const FaceTrackingCommand& b)
{
    return a.identity == b.identity && a.displayId == b.displayId && a.displayWidth == b.displayWidth &&
           a.displayHeight == b.displayHeight;
}

bool sameCommand(const FaceTrackingCommand& a, const FaceTrackingCommand& b)
{
    return a.revision == b.revision && a.lockGeneration == b.lockGeneration && a.identity == b.identity &&
           a.captureEpoch == b.captureEpoch && a.displayId == b.displayId && a.displayWidth == b.displayWidth &&
           a.captureContinuity == b.captureContinuity && a.displayHeight == b.displayHeight && a.window == b.window &&
           a.parentOriginX == b.parentOriginX && a.parentOriginY == b.parentOriginY && a.enabled == b.enabled &&
           a.minimumFacePixels == b.minimumFacePixels && sameCrop(a.crop, b.crop);
}

bool listed(const FaceTrackingCommand& command)
{
    if (!command.activeLocks) {
        return true;
    }
    const auto found = command.activeLocks->find(command.identity);
    return found != command.activeLocks->end() && found->second == command.lockGeneration;
}

bool validCommand(const FaceTrackingCommand& command)
{
    const auto bounds = lockRect(command.window);
    return listed(command) && command.identity != 0 && command.lockGeneration != 0 && command.displayWidth > 0 &&
           command.displayHeight > 0 && bounds.right <= command.displayWidth &&
           bounds.bottom <= command.displayHeight && std::isfinite(command.parentOriginX) &&
           std::isfinite(command.parentOriginY) && std::isfinite(command.minimumFacePixels) &&
           command.minimumFacePixels >= 1.0 &&
           command.minimumFacePixels <= std::max(command.displayWidth, command.displayHeight) &&
           face_tracking::validSelection(contextOf(command), command.crop, bounds);
}

bool validFrame(const FrameView& frame)
{
    const bool format = frame.format == PixelFormat::Bgra8 || frame.format == PixelFormat::Argb2101010;
    return frame.pixels && frame.width > 0 && frame.height > 0 && frame.strideBytes > 0 &&
           static_cast<int64_t>(frame.width) * 4 <= frame.strideBytes && frame.sourceWidth >= 0 &&
           frame.sourceHeight >= 0 &&
           static_cast<uint64_t>(frame.height) * frame.strideBytes <=
               static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) &&
           frame.sequence != 0 && std::isfinite(frame.stamp.receivedSeconds) && frame.stamp.receivedSeconds >= 0.0 &&
           format;
}

IntRect searchRect(const FaceAnchor& anchor, IntRect window, double reachWidths)
{
    // Command validation bounds all coordinates before integer conversion.
    const double reach = anchor.width * reachWidths;
    const int left = static_cast<int>(std::floor(std::max(static_cast<double>(window.x), anchor.centerX - reach)));
    const int top = static_cast<int>(std::floor(std::max(static_cast<double>(window.y), anchor.centerY - reach)));
    const int right =
        static_cast<int>(std::ceil(std::min(static_cast<double>(window.x) + window.width, anchor.centerX + reach)));
    const int bottom =
        static_cast<int>(std::ceil(std::min(static_cast<double>(window.y) + window.height, anchor.centerY + reach)));
    return {left, top, right - left, bottom - top};
}

bool reusableSearch(IntRect search, double referenceWidth, const FaceAnchor& anchor, IntRect window)
{
    // Preserve the detector's sampling grid through small changes. A new
    // search has 2.5 widths of reach. Requiring two widths around the face
    // reserves room for an admitted jump and the face extent while leaving
    // half a width of recentering hysteresis. Clipping to the parent avoids futile
    // recentering at an edge where no additional image is available.
    const auto required = lockRect(searchRect(anchor, window, 2.0));
    const auto available = lockRect(search);
    return anchor.width >= referenceWidth * (2.0 / 3.0) && anchor.width <= referenceWidth * 1.5 &&
           required.left >= available.left && required.top >= available.top && required.right <= available.right &&
           required.bottom <= available.bottom;
}

DetectionStatus statusOf(FaceDetectionStatus status)
{
    switch (status) {
    case FaceDetectionStatus::Completed:
        return DetectionStatus::Completed;
    case FaceDetectionStatus::Failed:
        return DetectionStatus::Failed;
    case FaceDetectionStatus::Unsupported:
        return DetectionStatus::Unsupported;
    }
    return DetectionStatus::Failed;
}

bool appendBoxes(const FaceDetectionResult& detected, IntRect roi, std::vector<LockRect>& boxes)
{
    boxes.reserve(detected.faces.size());
    for (const auto& face : detected.faces) {
        if (face.x < 0 || face.y < 0 || face.width <= 0 || face.height <= 0 ||
            static_cast<int64_t>(face.x) + face.width > roi.width ||
            static_cast<int64_t>(face.y) + face.height > roi.height) {
            return false;
        }
        const LockRect box{static_cast<double>(face.x) + roi.x, static_cast<double>(face.y) + roi.y,
                           static_cast<double>(face.x) + roi.x + face.width,
                           static_cast<double>(face.y) + roi.y + face.height};
        // Valid side-clipped boxes are deliberately excluded; malformed
        // coordinates instead fail the observation, even beside a valid face.
        if (face_lock::trustworthyBox(box, lockRect(roi))) {
            boxes.push_back(box);
        }
    }
    return true;
}

}  // namespace

FaceTrackingExchange::FaceTrackingExchange(std::function<void()> notify)
    : m_notify(std::move(notify))
{
}

void FaceTrackingExchange::select(const FaceTrackingCommand& command)
{
    std::lock_guard lock(m_mutex);
    if (!sameCommand(command, m_command)) {
        m_update.reset();
    }
    m_command = command;
}

FaceTrackingCommand FaceTrackingExchange::selection() const
{
    std::lock_guard lock(m_mutex);
    return m_command;
}

bool FaceTrackingExchange::matches(const FaceTrackingCommand& command) const
{
    std::lock_guard lock(m_mutex);
    return sameCommand(command, m_command);
}

bool FaceTrackingExchange::publish(FaceTrackingUpdate update)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_command.enabled || !listed(m_command) || update.revision != m_command.revision ||
            update.identity != m_command.identity || update.stamp.captureEpoch != m_command.captureEpoch ||
            update.stamp.displayId != m_command.displayId || update.decision.action == Action::Ignored) {
            return false;
        }
        update.serial = ++m_serial;
        m_update = update;
    }
    if (m_notify) {
        try {
            m_notify();
        } catch (...) {
            diagEmit(DiagChannel::FaceLock, "tracking notification failed");
        }
    }
    return true;
}

std::optional<FaceTrackingUpdate> FaceTrackingExchange::fetch(uint64_t& lastSeen) const
{
    std::lock_guard lock(m_mutex);
    if (!m_command.enabled || !listed(m_command) || !m_update || m_update->serial <= lastSeen ||
        m_update->revision != m_command.revision) {
        return {};
    }
    lastSeen = m_update->serial;
    return m_update;
}

FaceTrackingWorker::FaceTrackingWorker(std::shared_ptr<FaceTrackingExchange> exchange, FaceSessionFactory factory,
                                       std::function<double()> clock)
    : m_exchange(std::move(exchange)),
      m_factory(std::move(factory)),
      m_clock(std::move(clock))
{
}

bool FaceTrackingWorker::matchesSource(const FrameView& frame, const FaceTrackingCommand& command)
{
    return validFrame(frame) && frame.stamp.captureEpoch == command.captureEpoch &&
           frame.stamp.displayId == command.displayId && frame.displayWidth() == command.displayWidth &&
           frame.displayHeight() == command.displayHeight && !frame.cropped();
}

void FaceTrackingWorker::prunePolicies(const FaceTrackingCommand& command)
{
    if (command.activeLocks == m_activeLocks) {
        return;
    }
    m_activeLocks = command.activeLocks;
    if (!m_activeLocks) {
        return;
    }
    std::erase_if(m_policies, [this](const auto& item) {
        const auto found = m_activeLocks->find(item.first);
        return found == m_activeLocks->end() || found->second != item.second.command.lockGeneration;
    });
}

bool FaceTrackingWorker::preparePolicy(const FaceTrackingCommand& command, PolicyState& state)
{
    const auto* previous = &state.command;
    if (state.policy && command.lockGeneration < previous->lockGeneration) {
        return false;
    }
    if (!state.policy || command.lockGeneration != previous->lockGeneration) {
        state.policy.emplace(contextOf(command), command.crop, lockRect(command.window));
        state.search.reset();
        state.motion.reset(command.crop);
    }
    if (command.lockGeneration == previous->lockGeneration && !sameCommand(command, *previous)) {
        if (command.revision <= previous->revision) {
            return false;
        }
        if (!remapPolicy(command, state)) {
            return false;
        }
        state.motion.reset(command.crop);
    }
    state.command = command;
    return true;
}

bool FaceTrackingWorker::remapPolicy(const FaceTrackingCommand& command, PolicyState& state)
{
    if (!state.policy) {
        return false;
    }
    const auto& previous = state.command;
    auto& policy = *state.policy;
    const bool resumed = command.captureEpoch > previous.captureEpoch && command.captureContinuity != 0 &&
                         command.captureContinuity == previous.captureContinuity;
    const bool sameSource =
        sameSourceGrid(command, previous) && (command.captureEpoch == previous.captureEpoch || resumed);
    if (!sameSource) {
        state.search.reset();
        (void)policy.retire(contextOf(command), command.crop, lockRect(command.window));
    } else {
        const bool translated =
            command.window.width == previous.window.width && command.window.height == previous.window.height;
        if (policy
                .remap(contextOf(command), command.crop, lockRect(command.window),
                       command.parentOriginX - previous.parentOriginX, command.parentOriginY - previous.parentOriginY,
                       resumed)
                .action == Action::Ignored) {
            state.search.reset();
            return policy.retire(contextOf(command), command.crop, lockRect(command.window)).action != Action::Ignored;
        }
        if (!translated) {
            state.search.reset();
        } else if (state.search) {
            state.search->rect.x += command.window.x - previous.window.x;
            state.search->rect.y += command.window.y - previous.window.y;
        }
    }
    return true;
}

FaceDetectionResult FaceTrackingWorker::runDetector(const FrameView& crop, double minimumPixels)
{
    try {
        if (!m_factoryAttempted) {
            m_detector = m_factory ? m_factory() : nullptr;
            m_factoryAttempted = true;
        }
        return m_detector ? m_detector->detect(crop, minimumPixels)
                          : FaceDetectionResult{FaceDetectionStatus::Unsupported, {}};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return {FaceDetectionStatus::Failed, {}};
    }
}

face_tracking::Decision FaceTrackingWorker::detect(const FrameView& frame, const FaceTrackingCommand& command,
                                                   PolicyState& state)
{
    if (!state.policy) {
        return {};
    }
    auto& policy = *state.policy;
    if (!policy.current().following) {
        return policy.current();
    }
    const face_tracking::Stamp stamp{contextOf(command), frame.sequence, frame.stamp.receivedSeconds};
    const auto anchor = policy.current().anchor;
    if (!state.search || !reusableSearch(state.search->rect, state.search->faceWidth, anchor, command.window)) {
        state.search = SearchArea{searchRect(anchor, command.window, 2.5), anchor.width};
    }
    const IntRect roi = state.search->rect;
    FrameView crop = frame;
    crop.pixels = frame.rawPixelAt(roi.x, roi.y);
    crop.width = roi.width;
    crop.height = roi.height;
    crop.sourceX = roi.x;
    crop.sourceY = roi.y;
    crop.sourceWidth = frame.width;
    crop.sourceHeight = frame.height;
    const auto detected = runDetector(crop, command.minimumFacePixels);
    if (!m_exchange->matches(command)) {
        auto ignored = policy.current();
        ignored.action = Action::Ignored;
        ignored.reason = Reason::StaleContext;
        return ignored;
    }
    std::vector<LockRect> boxes;
    if (detected.status == FaceDetectionStatus::Completed && !appendBoxes(detected, roi, boxes)) {
        // An impossible box remains invalid rather than becoming a successful empty.
        const LockRect invalid{};
        return policy.advance(stamp, m_clock(), DetectionStatus::Completed, std::span(&invalid, 1));
    }
    return policy.advance(stamp, m_clock(), statusOf(detected.status), boxes);
}

FrameRegionResolution FaceTrackingWorker::resolve(const FrameRegionRequest& request)
{
    using Mode = FrameRegionResolution::Mode;
    if (!m_exchange || !m_clock) {
        return {Mode::Skip, {}, request.selectionRevision};
    }
    const FaceTrackingCommand command = m_exchange->selection();
    prunePolicies(command);
    if (command.revision != request.selectionRevision) {
        return {Mode::Skip, {}, request.selectionRevision};
    }
    if (!command.enabled) {
        // Preserve retirement across temporary disabling of this same lock.
        return {Mode::Configured, {}, request.selectionRevision};
    }
    if (!validCommand(command) || !matchesSource(request.frame, command)) {
        return {Mode::Skip, {}, request.selectionRevision};
    }
    // Allocate a new lock slot before publication. Its final commit cannot
    // allocate or throw after the consumer has observed the matching crop.
    static_assert(std::is_nothrow_move_assignable_v<PolicyState>);
    auto& current = m_policies[command.identity];
    auto candidate = current;
    if (!preparePolicy(command, candidate) || !candidate.policy) {
        return {Mode::Skip, {}, request.selectionRevision};
    }
    const double now = m_clock();
    if (!std::isfinite(now) || now < request.frame.stamp.receivedSeconds) {
        return {Mode::Skip, {}, request.selectionRevision};
    }
    auto& policy = *candidate.policy;
    auto decision = policy.current();
    if (request.freshFrame) {
        decision = detect(request.frame, command, candidate);
    } else {
        decision = policy.tick(contextOf(command), now);
    }
    const auto resolution = publishResolution(request, command, stabilize(command, decision, candidate));
    if (resolution.mode == Mode::Override) {
        current = std::move(candidate);
    }
    return resolution;
}

face_tracking::Decision FaceTrackingWorker::stabilize(const FaceTrackingCommand& command,
                                                      face_tracking::Decision decision, PolicyState& state)
{
    if (decision.action == Action::Ignored) {
        return decision;
    }
    if (decision.action == Action::Accepted) {
        decision.anchor = state.motion.follow(decision.anchor, decision.evidenceSourceSeconds);
    } else {
        if (decision.reason != Reason::Waiting) {
            state.motion.pause();
        }
        decision.anchor = state.motion.current();
    }
    // Raw policy geometry still drives matching and search. The border and
    // scope pass receive the same stabilized crop and evidence timestamp.
    decision.crop = face_lock::mapRegion(command.crop, decision.anchor);
    return decision;
}

FrameRegionResolution FaceTrackingWorker::publishResolution(const FrameRegionRequest& request,
                                                            const FaceTrackingCommand& command,
                                                            const face_tracking::Decision& decision)
{
    using Mode = FrameRegionResolution::Mode;
    if (decision.action == Action::Ignored || !m_exchange->matches(command)) {
        return {Mode::Skip, {}, request.selectionRevision};
    }
    const auto region = percentFromLockRect(decision.crop, command.displayWidth, command.displayHeight);
    const FaceTrackingUpdate update{
        0, command.revision, command.identity, request.frame.sequence, request.frame.stamp, decision, region};
    if (!m_exchange->publish(update) || !m_exchange->matches(command)) {
        return {Mode::Skip, {}, request.selectionRevision};
    }
    SS_DIAG(FaceLock,
            "track frame=%llu action=%.*s reason=%.*s epoch=%llu display=%u revision=%llu "
            "received=%.9f observed=%.9f fresh=%d roi=%.9f,%.9f,%.9f,%.9f",
            static_cast<unsigned long long>(request.frame.sequence),
            static_cast<int>(face_tracking::name(decision.action).size()), face_tracking::name(decision.action).data(),
            static_cast<int>(face_tracking::name(decision.reason).size()), face_tracking::name(decision.reason).data(),
            static_cast<unsigned long long>(request.frame.stamp.captureEpoch), request.frame.stamp.displayId,
            static_cast<unsigned long long>(command.revision), request.frame.stamp.receivedSeconds, m_clock(),
            request.freshFrame ? 1 : 0, region.leftPercent, region.topPercent, region.rightPercent,
            region.bottomPercent);
    return {Mode::Override, region, request.selectionRevision};
}

}  // namespace sidescopes
