#include "app/face_lock_controller.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "app/capture_controller.h"
#include "app/region_geometry.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"

extern "C" void glfwPostEmptyEvent(void);

namespace sidescopes {
namespace {

bool sameSource(const FaceTrackingCommand& a, const FaceTrackingCommand& b)
{
    return a.identity == b.identity && a.captureEpoch == b.captureEpoch && a.captureContinuity == b.captureContinuity &&
           a.displayId == b.displayId && a.displayWidth == b.displayWidth && a.displayHeight == b.displayHeight &&
           a.window == b.window && a.enabled == b.enabled;
}

IntRect clippedWindow(const AttachWindowRect& rect, const DisplayGeometry& display, AnalysisWorker::FrameSize frame)
{
    const double sx = frame.displayWidth / display.widthPoints;
    const double sy = frame.displayHeight / display.heightPoints;
    const double edges[] = {(rect.x - display.originX) * sx, (rect.y - display.originY) * sy,
                            (rect.x + rect.width - display.originX) * sx,
                            (rect.y + rect.height - display.originY) * sy};
    if (!std::all_of(std::begin(edges), std::end(edges), [](double value) { return std::isfinite(value); })) {
        return {};
    }
    const int left = static_cast<int>(std::ceil(std::clamp(edges[0], 0.0, double(frame.displayWidth))));
    const int top = static_cast<int>(std::ceil(std::clamp(edges[1], 0.0, double(frame.displayHeight))));
    const int right = static_cast<int>(std::floor(std::clamp(edges[2], 0.0, double(frame.displayWidth))));
    const int bottom = static_cast<int>(std::floor(std::clamp(edges[3], 0.0, double(frame.displayHeight))));
    return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

FaceLockState onFrameGrid(FaceLockState state, std::optional<std::pair<int, int>> original,
                          AnalysisWorker::FrameSize frame)
{
    if (!original || original->first <= 0 || original->second <= 0) {
        return state;
    }
    const double sx = double(frame.displayWidth) / original->first;
    const double sy = double(frame.displayHeight) / original->second;
    state.lastAnchor.centerX *= sx;
    state.lastAnchor.centerY *= sy;
    state.lastAnchor.width *= sx;
    state.offsetY *= sy / sx;
    state.sizeY *= sy / sx;
    return state;
}

bool validTrackedSelection(const FaceTrackingCommand& command)
{
    const auto& bounds = command.window;
    return std::isfinite(command.minimumFacePixels) && command.minimumFacePixels >= 1.0 &&
           command.minimumFacePixels <= std::max(command.displayWidth, command.displayHeight) &&
           face_tracking::validSelection(
               {command.revision, command.captureEpoch, command.displayId}, command.crop,
               {double(bounds.x), double(bounds.y), double(bounds.x) + bounds.width, double(bounds.y) + bounds.height});
}

}  // namespace

FaceLockController::FaceLockController(AttachController& attach, AnalysisWorker& worker, CaptureController& capture,
                                       std::function<double()> clock)
    : m_attach(attach),
      m_capture(capture),
      m_exchange(std::make_shared<FaceTrackingExchange>([] { glfwPostEmptyEvent(); }))
{
    const auto exchange = m_exchange;
    worker.setFrameRegionResolverFactory([exchange, clock = std::move(clock)] {
        const auto tracker = std::make_shared<FaceTrackingWorker>(exchange, createFaceDetectionSession, clock);
        return [tracker](const FrameRegionRequest& request) { return tracker->resolve(request); };
    });
}

void FaceLockController::invalidate()
{
    m_command.activeLocks = m_activeLocks;
    m_command.revision = ++m_revision;
    m_command.enabled = false;
    m_exchange->select(m_command);
}

void FaceLockController::refreshInventory()
{
    try {
        auto active = std::make_shared<std::map<uint64_t, uint64_t>>();
        for (const auto& [identity, lock] : m_locks) {
            active->emplace(identity, lock.generation);
        }
        m_activeLocks = std::move(active);
        m_inventoryDirty = false;
    } catch (const std::bad_alloc&) {
        // Keep the attached crop usable and retry the small control snapshot
        // later; an incomplete inventory must not enable stale face work.
        m_inventoryDirty = true;
    }
}

uint64_t FaceLockController::selectionRevision() const
{
    return m_revision;
}

void FaceLockController::addLock(uint64_t identity, FaceLockState state, std::optional<AttachWindowRect> windowRect,
                                 std::optional<std::pair<int, int>> coordinateSize)
{
    m_locks[identity] = Lock{state, windowRect, ++m_nextGeneration, {}, coordinateSize};
    refreshInventory();
    invalidate();
}

void FaceLockController::removeLock(uint64_t identity)
{
    if (m_locks.erase(identity) != 0) {
        refreshInventory();
        invalidate();
    }
}

void FaceLockController::clear()
{
    m_locks.clear();
    m_activeLocks = m_emptyLocks;
    m_inventoryDirty = false;
    invalidate();
}

void FaceLockController::activationChanged()
{
    invalidate();
}

void FaceLockController::rebindCrop(uint64_t identity, const RegionOfInterest& region,
                                    AnalysisWorker::FrameSize frameSize)
{
    const auto lock = m_locks.find(identity);
    if (lock == m_locks.end()) {
        return;
    }
    lock->second.state = onFrameGrid(lock->second.state, lock->second.coordinateSize, frameSize);
    lock->second.coordinateSize = {frameSize.displayWidth, frameSize.displayHeight};
    face_lock::rebindCrop(lock->second.state,
                          lockRectFromPercent(region, frameSize.displayWidth, frameSize.displayHeight));
    invalidate();
}

void FaceLockController::carryLockWithWindow(Lock& lock, const AttachWindowRect& rect,
                                             std::optional<AnalysisWorker::FrameSize> frameSize)
{
    if (lock.windowRect && frameSize && frameSize->coversDisplay()) {
        const bool sameSize = std::abs(rect.width - lock.windowRect->width) < 0.5 &&
                              std::abs(rect.height - lock.windowRect->height) < 0.5;
        if (sameSize) {
            if (const auto geometry = geometryOfDisplay(m_capture.capturedDisplay())) {
                const auto grid =
                    lock.coordinateSize.value_or(std::pair{frameSize->displayWidth, frameSize->displayHeight});
                const double dx = (rect.x - lock.windowRect->x) * grid.first / geometry->widthPoints;
                const double dy = (rect.y - lock.windowRect->y) * grid.second / geometry->heightPoints;
                if (std::isfinite(dx) && std::isfinite(dy)) {
                    face_lock::translate(lock.state, dx, dy);
                }
            }
        }
    }
    lock.windowRect = rect;
}

FaceTrackingCommand FaceLockController::makeCommand(const AttachDecision& decision,
                                                    std::optional<AnalysisWorker::FrameSize> frameSize,
                                                    bool gestureActive) const
{
    FaceTrackingCommand command;
    command.revision = m_revision;
    command.activeLocks = m_activeLocks;
    const auto lock = m_locks.find(decision.activeIdentity);
    if (lock == m_locks.end() || !decision.activeRect || !frameSize || !frameSize->coversDisplay() || gestureActive ||
        m_capture.dead() || m_capture.suspended() || m_inventoryDirty) {
        return command;
    }
    const auto geometry = geometryOfDisplay(m_capture.capturedDisplay());
    if (!geometry || geometry->widthPoints <= 0.0 || geometry->heightPoints <= 0.0) {
        return command;
    }
    const double sx = frameSize->displayWidth / geometry->widthPoints;
    command.window = clippedWindow(*decision.activeRect, *geometry, *frameSize);
    command.identity = decision.activeIdentity;
    command.lockGeneration = lock->second.generation;
    command.captureEpoch = m_capture.streamEpoch();
    command.captureContinuity = m_capture.continuityGeneration();
    command.displayId = m_capture.capturedDisplay();
    command.displayWidth = frameSize->displayWidth;
    command.displayHeight = frameSize->displayHeight;
    command.crop = onFrameGrid(lock->second.state, lock->second.coordinateSize, *frameSize);
    command.minimumFacePixels = 36.0 * sx;
    command.enabled = !command.window.empty();
    return command;
}

std::optional<RegionOfInterest> FaceLockController::acceptRegion(const FaceTrackingUpdate& update,
                                                                 const AttachDecision& decision)
{
    const auto geometry = geometryOfDisplay(update.stamp.displayId);
    if (!geometry || !decision.activeRect) {
        return {};
    }
    return m_attach.editRegion(
        update.region, *decision.activeRect,
        AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints});
}

FaceLockOutcome FaceLockController::consume(const AttachDecision& decision, double now)
{
    FaceLockOutcome outcome;
    const auto update = m_exchange->fetch(m_lastSeen);
    if (update && update->identity == decision.activeIdentity && m_locks.contains(update->identity)) {
        auto& lock = m_locks.at(update->identity);
        const auto action = update->decision.action;
        if (action == face_tracking::Action::Accepted) {
            lock.uncertainSince.reset();
            lock.state = onFrameGrid(
                lock.state, lock.coordinateSize,
                {m_command.displayWidth, m_command.displayHeight, m_command.displayWidth, m_command.displayHeight});
            lock.state.lastAnchor = update->decision.anchor;
            lock.coordinateSize = {m_command.displayWidth, m_command.displayHeight};
            outcome.applyRegion = acceptRegion(*update, decision);
        } else if (action == face_tracking::Action::OrdinaryAttached) {
            outcome.lostLock = update->identity;
        } else if (action == face_tracking::Action::Held && update->decision.reason != face_tracking::Reason::Waiting) {
            if (!lock.uncertainSince) {
                lock.uncertainSince = now;
            }
        }
    }
    // A failed fresh frame can be followed by a static occluded image, for
    // which capture withholds further frames. Its uncertainty still expires.
    const auto active = m_locks.find(m_command.identity);
    if (m_command.enabled && active != m_locks.end() && active->second.uncertainSince &&
        now - *active->second.uncertainSince >= 0.4) {
        outcome.lostLock = m_command.identity;
    }
    if (outcome.lostLock) {
        removeLock(*outcome.lostLock);
    }
    return outcome;
}

FaceLockOutcome FaceLockController::update(const AttachDecision& decision,
                                           std::optional<AnalysisWorker::FrameSize> frameSize, bool gestureActive,
                                           double now)
{
    if (m_inventoryDirty) {
        refreshInventory();
    }
    const auto before = m_locks.size();
    std::erase_if(m_locks, [this](const auto& entry) { return !m_attach.isAttached(entry.first); });
    if (m_locks.size() != before) {
        refreshInventory();
        invalidate();
    }
    const auto lock = m_locks.find(decision.activeIdentity);
    if (lock != m_locks.end() && decision.activeRect && !decision.windowMoving && !gestureActive) {
        carryLockWithWindow(lock->second, *decision.activeRect, frameSize);
    }
    auto command = makeCommand(decision, frameSize, gestureActive || decision.windowMoving);
    if (command.enabled && !validTrackedSelection(command)) {
        removeLock(command.identity);
        return {{}, command.identity};
    }
    if (!sameSource(command, m_command)) {
        command.revision = ++m_revision;
    } else if (command.revision == m_command.revision) {
        // Accepted worker anchors update the saved attachment, not its control
        // revision. Only explicit edits and window/source changes send a new
        // prior; ordinary video frames keep the worker's own live state.
        command.crop = m_command.crop;
    }
    m_command = command;
    m_exchange->select(m_command);
    return consume(decision, now);
}

bool FaceLockController::contains(uint64_t identity) const
{
    return m_locks.contains(identity);
}

bool FaceLockController::locked() const
{
    return !m_locks.empty();
}

}  // namespace sidescopes
