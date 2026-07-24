#include "app/face_lock_controller.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "app/capture_controller.h"
#include "app/region_geometry.h"
#include "core/diagnostics.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"

// Waking the main loop's idle wait so a finished probe is drained the moment
// it lands, not at the next timeout. Declared rather than pulling in the whole
// GLFW header: this unit compiles into the test binary too, which links no
// windowing library and provides its own stub.
extern "C" void glfwPostEmptyEvent(void);

namespace sidescopes {
namespace {

/// The face-lock probe cadence. The lock adopts only positions two
/// consecutive probes agree on, so this bounds how long the settle snap
/// trails the end of a pan or zoom; the ROI stays small enough that the
/// reference laptop shrugs it off.
constexpr double FaceLockProbeSeconds = 0.3;

/// The probe's reach around the last adopted anchor, in anchor widths:
/// covers the decision gates' proximity radius plus the face's own extent.
constexpr double FaceLockRoiWidths = 2.5;

/// The content-stability probe: mean absolute per-byte difference of the
/// region's sample grid that counts as "the content changed", and how long
/// the content must sit still before the border may show again. The
/// threshold sits just above capture noise: a pan over smooth skin moves
/// pixels only a little, and the border must hide on it instantly too.
constexpr double ContentChangeThreshold = 2.5;
constexpr double ContentSettleSeconds = 0.45;

bool sameRegionRect(const RegionOfInterest& a, const RegionOfInterest& b)
{
    return std::abs(a.leftPercent - b.leftPercent) < 0.01 && std::abs(a.topPercent - b.topPercent) < 0.01 &&
           std::abs(a.rightPercent - b.rightPercent) < 0.01 && std::abs(a.bottomPercent - b.bottomPercent) < 0.01;
}

// The probe's region of interest, in frame pixels: FaceLockRoiWidths anchor
// widths around the last adopted anchor, clipped to the attached window and
// the frame - a neighbouring window's faces are structurally out of reach.
// A lock that has lost its face searches the whole attached window instead.
IntRect faceLockRoi(const FaceLockState& lock, const AttachWindowRect& window, const DisplayGeometry& geometry,
                    int frameWidth, int frameHeight)
{
    const double scale = frameWidth / geometry.widthPoints;
    double left = (window.x - geometry.originX) * scale;
    double top = (window.y - geometry.originY) * scale;
    double right = left + window.width * scale;
    double bottom = top + window.height * scale;
    if (!face_lock::searchingWide(lock)) {
        const double reach = FaceLockRoiWidths * lock.lastAnchor.width;
        left = std::max(left, lock.lastAnchor.centerX - reach);
        top = std::max(top, lock.lastAnchor.centerY - reach);
        right = std::min(right, lock.lastAnchor.centerX + reach);
        bottom = std::min(bottom, lock.lastAnchor.centerY + reach);
    }
    left = std::max(left, 0.0);
    top = std::max(top, 0.0);
    right = std::min(right, static_cast<double>(frameWidth));
    bottom = std::min(bottom, static_cast<double>(frameHeight));

    return IntRect{static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left),
                   static_cast<int>(bottom - top)};
}

/// One probe's input, lifted out of a frame: the searched rectangle, the
/// frame's pixel density for the detector's density floor, and the rectangle's
/// pixels. The pixels are empty when the rectangle has no area.
struct ProbeCrop
{
    IntRect roi;
    float pixelsPerPoint = 1.0f;
    std::vector<uint8_t> pixels;
};

// Copies the lock's search rectangle out of the frame, tightly packed, so the
// detection thread reads pixels nothing else is writing.
ProbeCrop cropProbeRoi(const FrameView& view, const FaceLockState& lock, const AttachWindowRect& windowRect,
                       const DisplayGeometry& geometry)
{
    ProbeCrop crop;
    crop.roi = faceLockRoi(lock, windowRect, geometry, view.width, view.height);
    if (crop.roi.width <= 0 || crop.roi.height <= 0) {
        return crop;
    }
    crop.pixelsPerPoint = static_cast<float>(view.width / geometry.widthPoints);
    const std::size_t rowBytes = static_cast<std::size_t>(crop.roi.width) * 4;
    crop.pixels.resize(rowBytes * static_cast<std::size_t>(crop.roi.height));
    for (int row = 0; row < crop.roi.height; ++row) {
        std::memcpy(crop.pixels.data() + rowBytes * static_cast<std::size_t>(row),
                    view.bgra + static_cast<std::size_t>(crop.roi.y + row) * view.strideBytes +
                        static_cast<std::size_t>(crop.roi.x) * 4,
                    rowBytes);
    }

    return crop;
}

}  // namespace

FaceLockController::FaceLockController(AttachController& attach, AnalysisWorker& worker, CaptureController& capture)
    : m_attach(attach),
      m_worker(worker),
      m_capture(capture)
{
}

void FaceLockController::addLock(uint64_t identity, FaceLockState state, double verifiedAt,
                                 std::optional<AttachWindowRect> windowRect)
{
    m_locks[identity] = Lock{state, windowRect, verifiedAt};
}

void FaceLockController::removeLock(uint64_t identity)
{
    m_locks.erase(identity);
}

void FaceLockController::clear()
{
    m_locks.clear();
    m_hunting = false;
    m_contentChangedAt = -1.0;
}

void FaceLockController::onActivated(uint64_t identity, double now)
{
    // A face lock's anchor goes stale across a focus gap: dressing the border
    // from it flashes a wrong region for one probe's latency - invisible on a
    // fast detector, half a second on a slow one. Hold the border until this
    // activation's first verdict, probing now rather than waiting out the
    // cadence. A recently verified anchor (a fresh pick, a quick focus flip)
    // keeps its instant border.
    const auto activated = m_locks.find(identity);
    if (activated != m_locks.end() && now - activated->second.anchorVerifiedAt > FaceLockProbeSeconds) {
        m_hunting = true;
        m_nextProbe = 0.0;
    }
}

void FaceLockController::rebindCrop(uint64_t identity, const RegionOfInterest& region,
                                    AnalysisWorker::FrameSize frameSize)
{
    const auto lock = m_locks.find(identity);
    if (lock == m_locks.end()) {
        return;
    }
    face_lock::rebindCrop(lock->second.state, lockRectFromPercent(region, frameSize.width, frameSize.height));
}

// The per-frame face-lock step: prunes locks whose windows are gone, drains
// a finished probe, and starts the next one when the active window's lock
// is due. All adopt-or-hold judgement lives in face_lock::decide; this only
// moves data between the threads.
FaceLockOutcome FaceLockController::update(const AttachDecision& decision,
                                           std::optional<AnalysisWorker::FrameSize> frameSize,
                                           uint64_t activeWindowIdentity, const RegionOfInterest& analysisRegion,
                                           bool gestureActive, double now)
{
    std::erase_if(m_locks, [this](const auto& entry) { return !m_attach.isAttached(entry.first); });
    FaceLockOutcome outcome;
    if (m_probe.ready.load()) {
        std::vector<IntRect> boxes;
        {
            std::lock_guard guard(m_probe.mutex);
            boxes = std::move(m_probe.faces);
            m_probe.faces.clear();
        }
        m_probe.ready.store(false);
        outcome = ingestProbeResult(boxes, m_probe.roi, m_probe.forWindowIdentity, decision, activeWindowIdentity,
                                    frameSize, now);
    }
    const auto locked = m_locks.find(decision.activeIdentity);
    if (decision.activeIdentity == 0 || locked == m_locks.end()) {
        m_hunting = false;
        m_contentChangedAt = -1.0;

        return outcome;
    }
    if (decision.activeRect) {
        carryLockWithWindow(locked->second, *decision.activeRect, frameSize);
    }
    probeContentChange(analysisRegion, frameSize, now);
    // The probe never runs against a mid-gesture window: the user's drag
    // wins, and the lock catches up once things settle.
    if (gestureActive) {
        return outcome;
    }
    if (m_probe.running.load() || now < m_nextProbe) {
        return outcome;
    }
    m_nextProbe = now + FaceLockProbeSeconds;
    launchProbe(decision, locked->second.state);

    return outcome;
}

// Drains the finished probe and lets the pure core judge it against the
// gates. Every verdict is logged for grading; only an adoption returns a
// region, only a give-up returns a lost lock.
FaceLockOutcome FaceLockController::ingestProbeResult(const std::vector<IntRect>& boxes, IntRect roi,
                                                      uint64_t forIdentity, const AttachDecision& decision,
                                                      uint64_t activeWindowIdentity,
                                                      std::optional<AnalysisWorker::FrameSize> frameSize, double now)
{
    FaceLockOutcome outcome;
    const auto locked = m_locks.find(forIdentity);
    if (locked == m_locks.end() || forIdentity != decision.activeIdentity) {
        return outcome;
    }
    const LockRect roiRect{static_cast<double>(roi.x), static_cast<double>(roi.y),
                           static_cast<double>(roi.x + roi.width), static_cast<double>(roi.y + roi.height)};
    std::vector<FaceAnchor> candidates;
    candidates.reserve(boxes.size());
    std::size_t edgeDropped = 0;
    for (const IntRect& box : boxes) {
        const LockRect boxRect{static_cast<double>(box.x), static_cast<double>(box.y),
                               static_cast<double>(box.x + box.width), static_cast<double>(box.y + box.height)};
        if (!face_lock::trustworthyBox(boxRect, roiRect)) {
            ++edgeDropped;

            continue;
        }
        candidates.push_back(
            FaceAnchor{box.x + box.width / 2.0, box.y + box.height / 2.0, static_cast<double>(box.width)});
    }
    const bool wide = face_lock::searchingWide(locked->second.state);
    const FaceLockDecision verdict = face_lock::decide(locked->second.state, candidates);
    SS_DIAG(FaceLock, "%s reason='%s' wide=%d candidates=%zu edge-dropped=%zu anchor=%.1f,%.1f",
            verdict.adopt ? "adopt" : "hold", verdict.reason.c_str(), wide ? 1 : 0, candidates.size(), edgeDropped,
            locked->second.state.lastAnchor.centerX, locked->second.state.lastAnchor.centerY);
    m_hunting = verdict.hunting;
    if (!verdict.hunting) {
        locked->second.anchorVerifiedAt = now;
    }
    if (face_lock::givenUp(locked->second.state)) {
        SS_DIAG(FaceLock, "gave up - removing region");
        m_locks.erase(forIdentity);
        m_hunting = false;
        m_contentChangedAt = -1.0;
        outcome.lostLock = forIdentity;

        return outcome;
    }
    if (verdict.adopt) {
        outcome.applyRegion = mappedRegion(locked->second.state, frameSize, activeWindowIdentity);
    }

    return outcome;
}

// A window translation carries the face with it, so the anchors ride along
// and the probe keeps searching where the face actually is. A resize
// re-lays the content out unpredictably, so the anchors stay put and the
// probes re-find the face instead.
void FaceLockController::carryLockWithWindow(Lock& lock, const AttachWindowRect& rect,
                                             std::optional<AnalysisWorker::FrameSize> frameSize)
{
    if (!lock.windowRect) {
        lock.windowRect = rect;

        return;
    }
    const bool sameSize =
        std::abs(rect.width - lock.windowRect->width) < 0.5 && std::abs(rect.height - lock.windowRect->height) < 0.5;
    const double dx = rect.x - lock.windowRect->x;
    const double dy = rect.y - lock.windowRect->y;
    if (sameSize && (dx != 0.0 || dy != 0.0) && frameSize) {
        if (const auto geometry = geometryOfDisplay(m_capture.capturedDisplay())) {
            const double scale = frameSize->width / geometry->widthPoints;
            face_lock::translate(lock.state, dx * scale, dy * scale);
        }
    }
    lock.windowRect = rect;
}

// The grid the stability comparison runs on: three bytes per tap, taken at the
// centre of each cell of an even grid over the region.
std::vector<uint8_t> FaceLockController::sampleContentGrid(const FrameView& view, const RegionOfInterest& region)
{
    constexpr int GridSide = 16;

    std::vector<uint8_t> samples;
    samples.reserve(static_cast<std::size_t>(GridSide) * GridSide * 3);
    const double left = region.leftPercent / 100.0 * view.width;
    const double top = region.topPercent / 100.0 * view.height;
    const double width = (region.rightPercent - region.leftPercent) / 100.0 * view.width;
    const double height = (region.bottomPercent - region.topPercent) / 100.0 * view.height;
    for (int gridY = 0; gridY < GridSide; ++gridY) {
        for (int gridX = 0; gridX < GridSide; ++gridX) {
            const int px = std::clamp(static_cast<int>(left + (gridX + 0.5) * width / GridSide), 0, view.width - 1);
            const int py = std::clamp(static_cast<int>(top + (gridY + 0.5) * height / GridSide), 0, view.height - 1);
            const uint8_t* pixel = view.pixelAt(px, py);
            samples.push_back(pixel[0]);
            samples.push_back(pixel[1]);
            samples.push_back(pixel[2]);
        }
    }

    return samples;
}

// A cheap content-stability probe over the face-locked region: a sparse grid
// of pixels compared frame to frame. Any considerable change - a pan, a zoom,
// even a develop-slider drag - hides the border until the content settles;
// the scopes keep analyzing the region throughout. A region rectangle we
// moved ourselves only refreshes the baseline.
void FaceLockController::probeContentChange(const RegionOfInterest& region,
                                            std::optional<AnalysisWorker::FrameSize> frameSize, double now)
{
    if (!frameSize) {
        return;
    }
    std::vector<uint8_t> samples;
    const bool sampled =
        m_worker.withLatestFrame([&](const FrameView& view) { samples = sampleContentGrid(view, region); });
    if (!sampled || samples.empty()) {
        return;
    }
    if (sameRegionRect(region, m_contentRect) && samples.size() == m_contentSamples.size()) {
        long long total = 0;
        for (std::size_t index = 0; index < samples.size(); ++index) {
            total += std::abs(static_cast<int>(samples[index]) - static_cast<int>(m_contentSamples[index]));
        }
        if (static_cast<double>(total) / static_cast<double>(samples.size()) > ContentChangeThreshold) {
            m_contentChangedAt = now;
        }
    }
    m_contentRect = region;
    m_contentSamples = std::move(samples);
}

bool FaceLockController::contains(uint64_t identity) const
{
    return m_locks.contains(identity);
}

bool FaceLockController::hunting() const
{
    return m_hunting;
}

bool FaceLockController::contentUnsettled(double now) const
{
    return m_contentChangedAt >= 0.0 && now - m_contentChangedAt < ContentSettleSeconds;
}

bool FaceLockController::probeRunning() const
{
    return m_probe.running.load();
}

// Copies the probe's region of interest out of the latest frame and hands
// it to a detached detection thread, so detection never hitches a frame.
void FaceLockController::launchProbe(const AttachDecision& decision, const FaceLockState& lock)
{
    const auto geometry = geometryOfDisplay(m_capture.capturedDisplay());
    if (!geometry || !decision.activeRect) {
        return;
    }
    ProbeCrop crop;
    const bool copied = m_worker.withLatestFrame(
        [&](const FrameView& view) { crop = cropProbeRoi(view, lock, *decision.activeRect, *geometry); });
    if (!copied || crop.pixels.empty()) {
        return;
    }
    auto pixels = std::make_shared<std::vector<uint8_t>>(std::move(crop.pixels));
    const IntRect roi = crop.roi;
    const float pixelsPerPoint = crop.pixelsPerPoint;
    m_probe.forWindowIdentity = decision.activeIdentity;
    m_probe.roi = roi;
    m_probe.running.store(true);
    Probe* probe = &m_probe;
    std::thread([probe, pixels, roi, pixelsPerPoint] {
        FrameView view;
        view.bgra = pixels->data();
        view.strideBytes = roi.width * 4;
        view.width = roi.width;
        view.height = roi.height;
        std::vector<IntRect> faces = detectFaces(view, pixelsPerPoint);
        for (IntRect& box : faces) {
            box.x += roi.x;
            box.y += roi.y;
        }
        {
            std::lock_guard lock(probe->mutex);
            probe->faces = std::move(faces);
        }
        probe->ready.store(true);
        // The wake goes out before the running flag clears: the shutdown
        // drain waits on running, and GLFW must still be alive to hear it.
        glfwPostEmptyEvent();
        probe->running.store(false);
    }).detach();
}

// The adopted anchor's region, mapped through the same path as a border
// edit: the stored screen-glued rectangle re-derived so the analysis region
// follows. Nothing to apply when the frame, display, or window geometry is
// missing, or the window is minimized.
std::optional<RegionOfInterest> FaceLockController::mappedRegion(const FaceLockState& lock,
                                                                 std::optional<AnalysisWorker::FrameSize> frameSize,
                                                                 uint64_t activeWindowIdentity)
{
    if (!frameSize || frameSize->width <= 0 || frameSize->height <= 0) {
        return std::nullopt;
    }
    const auto geometry = geometryOfDisplay(m_capture.capturedDisplay());
    const auto windowGeom = windowGeometry(activeWindowIdentity);
    if (!geometry || !windowGeom || windowGeom->minimized) {
        return std::nullopt;
    }
    const LockRect target = face_lock::mapRegion(lock, lock.lastAnchor);
    const RegionOfInterest edited = percentFromLockRect(target, frameSize->width, frameSize->height);

    return m_attach.editRegion(
        edited, AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height},
        AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints});
}

}  // namespace sidescopes
