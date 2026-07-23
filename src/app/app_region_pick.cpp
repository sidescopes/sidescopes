#include "app/app.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/region_geometry.h"
#include "app/window_suggestions.h"
#include "core/diagnostics.h"
#include "core/region_suggestions.h"
#include "platform/face_detection.h"

namespace sidescopes {
namespace {

/// The outcome of scanning one non-streamed display: its detector boxes, the
/// grabbed frame's size (zero when the grab failed), and how long grab plus
/// detect took, for the diagnostics line.
struct ScanResult
{
    std::vector<IntRect> faces;
    int width = 0;
    int height = 0;
    double elapsedMs = 0.0;
};

// Grabs one display off the capture stream and runs the face detector on it.
// Pure of application state: it takes only the display and its point width
// (for the detector's density floor), so it is safe to run on a detached
// thread with nothing but value captures.
ScanResult scanDisplayForFaces(uint32_t displayId, double widthPoints)
{
    const auto started = std::chrono::steady_clock::now();
    ScanResult result;
    if (const std::optional<CapturedImage> image = captureDisplayImage(displayId)) {
        FrameView view;
        view.bgra = image->bgra.data();
        view.strideBytes = image->width * 4;
        view.width = image->width;
        view.height = image->height;
        const float pixelsPerPoint = widthPoints > 0.0 ? static_cast<float>(image->width / widthPoints) : 1.0f;
        result.faces = detectFaces(view, pixelsPerPoint);
        result.width = image->width;
        result.height = image->height;
    }
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started;
    result.elapsedMs = elapsed.count();

    return result;
}

// The on-screen windows of a display become its region suggestions. The
// platform query lives here; buildWindowSuggestions owns the choice of which
// windows to suggest and in what order.
std::vector<SuggestedRegion> windowSuggestionsFor(uint32_t displayId)
{
    const auto geometry = geometryOfDisplay(displayId);
    if (!geometry) {
        return {};
    }

    // The most windows the picker suggests at once, so the overlay stays
    // readable on a crowded desktop.
    constexpr int MaxWindowSuggestions = 5;

    return buildWindowSuggestions(onScreenWindows(displayId), *geometry, MaxWindowSuggestions);
}

}  // namespace

void App::handleRegionPicking()
{
    if (m_regionPicking && m_wantRegionPick) {
        const bool targetPin = *m_wantRegionPick == RegionPickerMode::PinColor;
        if (targetPin || m_regionPickIsPin) {
            // Region picking and color pinning are separate tools; a pick never
            // morphs across that boundary. The active pick closes with no effect
            // on the region, and the requested tool opens fresh once it is gone.
            m_regionPickSwallowCancel = true;
            cancelRegionPick();
        } else {
            // The toolbar keeps working mid-pick: choosing a region tool
            // switches the active picker's mode instead of stacking one.
            setRegionPickMode(*m_wantRegionPick);
            m_wantRegionPick.reset();
        }
    }
    if (!m_regionPicking && m_wantRegionPick && m_captureController.capturedDisplay() != 0) {
        openRegionPicker(*m_wantRegionPick);
    }
}

void App::openRegionPicker(RegionPickerMode mode)
{
    hideRegionBorder();
    // The previous region's border must not leak into the analyzed frame: its
    // strokes read as rectangle edges and cut suggestions short. Wait briefly
    // for a frame taken after the border left the screen.
    if (!isFullScreen()) {
        waitForBorderFreeFrame();
    }
    const std::vector<PickerDisplay> pickerDisplays = buildPickerDisplays();
    logPickerSuggestions(pickerDisplays);
    if (beginRegionPick(pickerDisplays, mode)) {
        m_regionPicking = true;
        m_regionPickIsPin = mode == RegionPickerMode::PinColor;
        // The streamed display's faces opened with the picker; scan the rest
        // in the background and deliver them as they land.
        launchDisplayFaceScans(pickerDisplays);
    }
    // Consumed either way: a request that could not open must not retry every
    // frame.
    m_wantRegionPick.reset();
    m_lastActivity = glfwGetTime();
}

void App::waitForBorderFreeFrame()
{
    // The 60 ms floor outlasts an in-flight pre-hide frame's capture-to-delivery;
    // the 300 ms cap keeps the picker responsive if the stream has stalled.
    uint64_t staleSequence = 0;
    (void)m_worker.withLatestFrame([&](const FrameView& view) { staleSequence = view.sequence; });
    const double hiddenAt = glfwGetTime();
    for (;;) {
        const double elapsed = glfwGetTime() - hiddenAt;
        if (elapsed >= 0.3) {
            break;
        }
        uint64_t sequence = staleSequence;
        (void)m_worker.withLatestFrame([&](const FrameView& view) { sequence = view.sequence; });
        // Inequality, not greater-than: a freshly switched stream counts its
        // frames from one again.
        if (sequence != staleSequence && elapsed >= 0.06) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

std::vector<PickerDisplay> App::buildPickerDisplays()
{
    // The suggestions, per display: the visible application windows, frontmost
    // first, plus, behind their own key, the faces the platform detector
    // finds. The streamed display's faces come from its live frame right
    // now; the other displays are scanned in the background (see
    // launchDisplayFaceScans) and their suggestions arrive later.
    const uint32_t streamed = m_captureController.capturedDisplay();
    std::vector<SuggestedRegion> faceSuggestions;
    m_faceCandidates.clear();
    if (supportsFaceDetection()) {
        (void)m_worker.withLatestFrame([&](const FrameView& view) {
            const auto geometry = geometryOfDisplay(streamed);
            const float pixelsPerPoint = geometry ? static_cast<float>(view.width / geometry->widthPoints) : 1.0f;
            const std::vector<IntRect> faces = detectFaces(view, pixelsPerPoint);
            faceSuggestions = buildFaceSuggestions(faces, view.width, view.height);
            // The raw boxes are remembered too, one candidate each: a confirmed
            // face pick anchors its lock on the detector's box, not the inset.
            const std::vector<FaceCandidate> candidates = buildFaceCandidates(faces, streamed, view.width, view.height);
            m_faceCandidates.insert(m_faceCandidates.end(), candidates.begin(), candidates.end());
            SS_DIAG(Suggestions, "display %u faces=%zu (streamed)", streamed, faceSuggestions.size());
        });
    }

    std::vector<PickerDisplay> pickerDisplays;
    // Remember every window and its identity so a confirmed window pick can
    // be turned into an attachment.
    m_windowCandidates.clear();
    for (const CaptureTarget& target : m_capture->listTargets()) {
        PickerDisplay entry;
        entry.displayId = target.displayId;
        entry.windows = windowSuggestionsFor(target.displayId);
        if (const auto geometry = geometryOfDisplay(target.displayId)) {
            for (const DesktopWindow& onScreen : onScreenWindows(target.displayId)) {
                const WindowGeometry rect{onScreen.x, onScreen.y, onScreen.width, onScreen.height, false, {}};
                m_windowCandidates.push_back({onScreen.windowIdentity, onScreen.ownerPid, onScreen.application,
                                              AttachWindowRect{onScreen.x, onScreen.y, onScreen.width, onScreen.height},
                                              displayPercentRect(rect, *geometry), target.displayId});
            }
        }
        if (target.displayId == streamed) {
            // The streamed display's faces are known now, from the live frame:
            // the picker opens with them and, empty or not, its scan is done.
            entry.faces = faceSuggestions;
            entry.facesScanned = true;
        }
        pickerDisplays.push_back(std::move(entry));
    }

    return pickerDisplays;
}

// One detached scan per non-streamed display, following the face-lock probe's
// discipline: a per-scan record the thread fills under a mutex, a ready flag,
// a wake, and a running flag the shutdown drain waits on. Opening the picker
// never blocks on these - only the streamed display's instant scan gates the
// open.
void App::launchDisplayFaceScans(const std::vector<PickerDisplay>& pickerDisplays)
{
    if (!supportsFaceDetection()) {
        return;
    }
    ++m_facePickGeneration;
    // Clear out the records of scans that have finished and been consumed; a
    // still-running one stays put - its detached thread holds a pointer in.
    std::erase_if(m_displayFaceScans, [](const std::unique_ptr<DisplayFaceScan>& scan) {
        return !scan->running.load() && !scan->ready.load();
    });
    const uint32_t streamed = m_captureController.capturedDisplay();
    for (const PickerDisplay& entry : pickerDisplays) {
        if (entry.displayId == streamed) {
            continue;  // scanned already, from the live frame
        }
        const auto geometry = geometryOfDisplay(entry.displayId);
        const double widthPoints = geometry ? geometry->widthPoints : 0.0;
        m_displayFaceScans.push_back(std::make_unique<DisplayFaceScan>());
        DisplayFaceScan* scan = m_displayFaceScans.back().get();
        scan->displayId = entry.displayId;
        scan->generation = m_facePickGeneration;
        scan->running.store(true);
        const uint32_t displayId = entry.displayId;
        std::thread([scan, displayId, widthPoints] {
            ScanResult scanned = scanDisplayForFaces(displayId, widthPoints);
            {
                std::lock_guard lock(scan->mutex);
                scan->faces = std::move(scanned.faces);
                scan->frameWidth = scanned.width;
                scan->frameHeight = scanned.height;
                scan->elapsedMs = scanned.elapsedMs;
            }
            scan->ready.store(true);
            // The wake goes out before the running flag clears: the shutdown
            // drain waits on running, and GLFW must still be alive to hear it.
            glfwPostEmptyEvent();
            scan->running.store(false);
        }).detach();
    }
}

// Drains every finished display scan into the open picker. Runs each frame,
// so a scan that lands after its picker closed is still consumed (and
// dropped) and its record retired.
void App::drainDisplayFaceScans()
{
    for (const std::unique_ptr<DisplayFaceScan>& scan : m_displayFaceScans) {
        if (scan->ready.load()) {
            consumeDisplayFaceScan(*scan);
            scan->ready.store(false);
        }
    }
    // Only a fully finished scan is removed: a running one's detached thread
    // still holds a pointer into it.
    std::erase_if(m_displayFaceScans, [](const std::unique_ptr<DisplayFaceScan>& scan) {
        return !scan->running.load() && !scan->ready.load();
    });
}

// Turns one landed scan into the display's face suggestions: the overlay
// boxes and the face candidates for a confirmed pick, both keyed to this
// display's own frame dimensions. A scan whose picker has closed or that a
// newer opening superseded is logged and dropped.
void App::consumeDisplayFaceScan(DisplayFaceScan& scan)
{
    std::vector<IntRect> boxes;
    int frameWidth = 0;
    int frameHeight = 0;
    double elapsedMs = 0.0;
    {
        std::lock_guard lock(scan.mutex);
        boxes = std::move(scan.faces);
        scan.faces.clear();
        frameWidth = scan.frameWidth;
        frameHeight = scan.frameHeight;
        elapsedMs = scan.elapsedMs;
    }
    SS_DIAG(Suggestions, "display %u scan faces=%zu elapsed=%.1fms", scan.displayId, boxes.size(), elapsedMs);
    if (!m_regionPicking || scan.generation != m_facePickGeneration) {
        return;
    }
    const std::vector<SuggestedRegion> suggestions = buildFaceSuggestions(boxes, frameWidth, frameHeight);
    const std::vector<FaceCandidate> candidates = buildFaceCandidates(boxes, scan.displayId, frameWidth, frameHeight);
    m_faceCandidates.insert(m_faceCandidates.end(), candidates.begin(), candidates.end());
    // Delivering the suggestions marks this display scanned: an empty list now
    // reads as "none found", and a failed grab (empty too) shows no boxes.
    updatePickerFaces(scan.displayId, suggestions);
}

// Recovers which window candidate a confirmed region names. The picker passes
// a window's exact rectangle through unchanged, so the match is a near-exact
// rectangle comparison; a freehand draw matches nothing.
const App::WindowCandidate* App::matchWindowCandidate(uint32_t displayId, const RegionOfInterest& region) const
{
    const WindowCandidate* best = nullptr;
    double bestDelta = 0.0;
    for (const WindowCandidate& candidate : m_windowCandidates) {
        if (candidate.displayId != displayId) {
            continue;
        }
        const double delta = std::abs(candidate.region.leftPercent - region.leftPercent) +
                             std::abs(candidate.region.topPercent - region.topPercent) +
                             std::abs(candidate.region.rightPercent - region.rightPercent) +
                             std::abs(candidate.region.bottomPercent - region.bottomPercent);
        if (best == nullptr || delta < bestDelta) {
            best = &candidate;
            bestDelta = delta;
        }
    }
    // One percent of summed edge error tolerates the picker's coordinate
    // round-trip while rejecting a hand-drawn rectangle.
    constexpr double MatchTolerance = 1.0;

    return (best != nullptr && bestDelta <= MatchTolerance) ? best : nullptr;
}

void App::logPickerSuggestions(const std::vector<PickerDisplay>& pickerDisplays)
{
    // Field diagnosis: exactly what the pipeline suggested, one line per
    // suggested window.
    if (!diagEnabled(DiagChannel::Suggestions)) {
        return;
    }
    for (const auto& entry : pickerDisplays) {
        for (const auto& suggestion : entry.windows) {
            SS_DIAG(Suggestions, "display %u suggestion '%s' %.1f,%.1f..%.1f,%.1f%%", entry.displayId,
                    suggestion.label.c_str(), suggestion.region.leftPercent, suggestion.region.topPercent,
                    suggestion.region.rightPercent, suggestion.region.bottomPercent);
        }
    }
}

void App::handleRegionBorderEdit()
{
    // The region border is live: dragging its edges, corners, or move tab
    // adjusts the region it currently outlines - the attached region of the
    // focused attached window, or the global one - with the scopes following.
    if (m_regionPicking) {
        m_attachBorderEditing = false;

        return;
    }
    const RegionBorderEdit edit = pollRegionBorderEdit();
    if (edit.editing && !m_attachBorderEditing) {
        // Latch what the border showed when the drag began: no focus race
        // can reroute the edit to the other region kind.
        m_attachBorderEditIdentity = m_activeWindowIdentity;
    }
    // While an attached border is dragged, a click-through veil dims
    // everything outside its window - the resize limit made visible.
    if (edit.editing && m_attachBorderEditIdentity != 0 && m_attachBorderEditIdentity == m_activeWindowIdentity) {
        const auto windowGeom = windowGeometry(m_activeWindowIdentity);
        const auto geometry = geometryOfDisplay(m_captureController.capturedDisplay());
        if (windowGeom && geometry) {
            showAttachedEditDim(m_captureController.capturedDisplay(), displayPercentRect(*windowGeom, *geometry));
        }
    } else if (!edit.editing && m_attachBorderEditing) {
        hideAttachedEditDim();
    }
    m_attachBorderEditing = edit.editing;
    if (edit.dismissed) {
        dismissEditedBorder();
    } else if (edit.attachToggled) {
        toggleRegionAttach();
    } else if (edit.region) {
        applyBorderEdit(*edit.region);
    }
}

// The border's attach toggle. An attached region lets go of its window and
// becomes the global region in place; a global one attaches to the
// frontmost window under it. Explicit conversions only - the structural
// no-conversion rule is about drags and focus races, never this button.
void App::toggleRegionAttach()
{
    if (regionKind() == RegionKind::Attached) {
        const RegionOfInterest region = m_analysis.region;
        m_attach.detachAll();
        m_faceLocks.clear();
        m_faceLockHunting = false;
        m_regionContentChangedAt = -1.0;
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
        m_globalRegion = region;
        setRegion(region);
    } else {
        attachGlobalRegionToWindow();
    }
    m_lastActivity = glfwGetTime();
    syncRegionBorder();
}

// Attaching a global region: the frontmost on-screen window under the
// region's centre becomes its window, the rectangle staying exactly where
// it is. Over no window at all this is a no-op - predictable beats
// guessing a target.
void App::attachGlobalRegionToWindow()
{
    const uint32_t displayId = m_captureController.capturedDisplay();
    const auto geometry = geometryOfDisplay(displayId);
    if (!geometry || isFullScreen()) {
        return;
    }
    const RegionOfInterest region = m_globalRegion;
    const double centerX = (region.leftPercent + region.rightPercent) / 2.0;
    const double centerY = (region.topPercent + region.bottomPercent) / 2.0;
    for (const DesktopWindow& window : attachCandidateWindows(displayId)) {
        const WindowGeometry rect{window.x, window.y, window.width, window.height, false, {}};
        const RegionOfInterest windowRegion = displayPercentRect(rect, *geometry);
        if (centerX < windowRegion.leftPercent || centerX > windowRegion.rightPercent ||
            centerY < windowRegion.topPercent || centerY > windowRegion.bottomPercent) {
            continue;
        }
        adoptAttachedPick(window.windowIdentity, window.ownerPid,
                          m_attach.attach(window.windowIdentity, window.ownerPid, window.application,
                                          AttachWindowRect{window.x, window.y, window.width, window.height},
                                          AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints,
                                                            geometry->heightPoints},
                                          region));

        return;
    }
}

// The border's close affordances dismiss the region it outlines: the
// attached one detaches from its window only, the global one resets to
// full screen; the other attached windows keep their regions either way.
void App::dismissEditedBorder()
{
    if (regionKind() == RegionKind::Attached) {
        m_attach.remove(m_activeWindowIdentity);
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        setRegion(m_globalRegion);
    } else {
        m_globalRegion = RegionOfInterest{};
        setRegion(m_globalRegion);
    }
    m_lastActivity = glfwGetTime();
}

// Routes a border drag to the region kind it began on. An edit that began
// on an attached border may NEVER fall through to the global region - no
// accidental conversion; if the attached routing cannot resolve, the edit
// is dropped instead.
void App::applyBorderEdit(const RegionOfInterest& edited)
{
    RegionOfInterest applied = edited;
    if (m_attachBorderEditIdentity != 0) {
        if (m_attachBorderEditIdentity != m_activeWindowIdentity) {
            return;
        }
        const auto geometry = geometryOfDisplay(m_captureController.capturedDisplay());
        const auto windowGeom = windowGeometry(m_activeWindowIdentity);
        if (!geometry || !windowGeom) {
            return;
        }
        // Attached: re-derive the window-relative fraction so the region
        // keeps following its window.
        applied = m_attach.editRegion(
            edited, AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height},
            AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints});
        // A face-locked window's edit re-teaches the lock: the new rectangle
        // becomes the crop the face carries from here on.
        const auto lock = m_faceLocks.find(m_attachBorderEditIdentity);
        if (lock != m_faceLocks.end() && m_frameSize) {
            face_lock::rebindCrop(lock->second.state,
                                  lockRectFromPercent(applied, m_frameSize->width, m_frameSize->height));
        }
    } else {
        m_globalRegion = edited;
    }
    m_analysis.region = applied;
    // The analysis-dirty path syncs the border this same iteration.
    m_analysisDirty = true;
    m_lastActivity = glfwGetTime();
}

void App::pollActiveRegionPick()
{
    // While the picker is up, whatever the user indicates previews on the scopes
    // immediately; confirmation keeps it, Esc restores.
    if (!m_regionPicking) {
        return;
    }
    const RegionPickPoll poll = pollRegionPick();
    if (poll.pinMode) {
        pollPinPick(poll);
    } else {
        pollRegionPreview(poll);
    }
}

void App::pollPinPick(const RegionPickPoll& poll)
{
    // Color pinning never touches the region: clicks and drags deliver samples
    // to average, and a finish just puts things back.
    std::optional<FloatColor> chip;
    if (const auto cursor = globalCursorPosition()) {
        if (displayAtPoint(*cursor).value_or(0) == m_captureController.capturedDisplay() &&
            !m_captureController.dead() && m_frameSize) {
            if (const auto geometry = geometryOfDisplay(m_captureController.capturedDisplay())) {
                // The chip previews exactly what a click will pin: the same point
                // sample the live cursor readout takes, not an averaged patch.
                const int pixelX =
                    static_cast<int>((cursor->x - geometry->originX) * m_frameSize->width / geometry->widthPoints);
                const int pixelY =
                    static_cast<int>((cursor->y - geometry->originY) * m_frameSize->height / geometry->heightPoints);
                chip = m_worker.sampleFrameColor(pixelX, pixelY);
            }
        } else {
            // Another display: the throttled one-shot sampler already tracks the
            // cursor there.
            std::lock_guard lock(m_screenSample->mutex);
            chip = m_screenSample->color;
        }
    }
    setRegionPickChipColor(chip);

    if (poll.pinnedPoint || poll.pinnedSample) {
        applyPinnedColor(poll);
    }
    if (poll.finished || !poll.active) {
        m_regionPicking = false;
        m_regionPickSwallowCancel = false;
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
}

void App::applyPinnedColor(const RegionPickPoll& poll)
{
    std::optional<FloatColor> pinned;
    if (poll.displayId == m_captureController.capturedDisplay() && !m_captureController.dead()) {
        if (poll.pinnedPoint && m_frameSize) {
            // A plain pin samples the frame exactly like the live readout; only a
            // dragged rectangle averages, the explicit way to ask for a swatch.
            const int pixelX = static_cast<int>(poll.pinnedPoint->xPercent / 100.0 * m_frameSize->width);
            const int pixelY = static_cast<int>(poll.pinnedPoint->yPercent / 100.0 * m_frameSize->height);
            pinned = m_worker.sampleFrameColor(pixelX, pixelY);
        } else if (poll.pinnedSample) {
            pinned = averageFrameColor(*poll.pinnedSample);
        }
    } else {
        std::lock_guard lock(m_screenSample->mutex);
        pinned = m_screenSample->color;
    }
    if (pinned) {
        m_pins.pin(*pinned);
    }
    // The click's own Shift decided: pin-and-continue stays, a plain pin ends
    // the errand.
    if (!poll.pinnedKeepOpen) {
        cancelRegionPick();
    }
    m_lastActivity = glfwGetTime();
}

void App::pollRegionPreview(const RegionPickPoll& poll)
{
    const auto applyRegion = [&](const RegionOfInterest& region) {
        if (region.leftPercent == m_analysis.region.leftPercent && region.topPercent == m_analysis.region.topPercent &&
            region.rightPercent == m_analysis.region.rightPercent &&
            region.bottomPercent == m_analysis.region.bottomPercent) {
            return;
        }
        m_analysis.region = region;
        m_analysisDirty = true;
        m_lastActivity = glfwGetTime();
    };
    // Live preview only for the captured display: previewing a suggestion on
    // another display would flap the capture stream on every hover.
    if (poll.preview && poll.displayId == m_captureController.capturedDisplay()) {
        applyRegion(*poll.preview);
    }
    if (poll.finished || !poll.active) {
        m_regionPicking = false;
        if (poll.confirmed) {
            if (poll.displayId != 0 && poll.displayId != m_captureController.capturedDisplay()) {
                m_captureController.requestDisplay(poll.displayId);
                m_captureController.start();
            }
            confirmPickedRegion(poll);
        } else if (!m_regionPickSwallowCancel) {
            // Cancelled with Esc: detach every attached window and reset all
            // drawing to full screen. A cancel ordered by a tool switch is
            // not the user's Esc and resets nothing.
            resetToFullScreen();
        }
        m_regionPickSwallowCancel = false;
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
}

// The shared tail of both attached creations: the global region retires
// (one region kind at a time), the motion state starts fresh, the watch
// rebinds on the next follow step, and the picked window comes up so the
// border never wraps someone else's pixels.
void App::adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region)
{
    m_globalRegion = RegionOfInterest{};
    // A manual pick or draw replaces whatever face lock the window wore.
    m_faceLocks.erase(identity);
    m_attachedWindowMoving = false;
    m_attachGripActive = false;
    m_attachRegionMovedAt = -1.0;
    unwatchWindowMotion();
    m_activeWindowIdentity = 0;
    raiseWindow(identity, ownerPid);
    setRegion(region);
}

}  // namespace sidescopes
