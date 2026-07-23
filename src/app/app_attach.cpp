#include "app/app.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "app/border_label.h"
#include "app/face_lock.h"
#include "app/region_geometry.h"
#include "core/diagnostics.h"

namespace sidescopes {
namespace {

// The border returns this long after the active window last moved and the
// grip released - what keeps a slow drag's sparse updates from flickering it.
constexpr double AttachMotionSettleSeconds = 0.2;

}  // namespace

// Gathers this frame's observation for every attached window: geometry,
// minimized state, and - for visible ones - the display it sits on.
std::vector<AttachedWindowObservation> App::gatherAttachedObservations() const
{
    std::vector<AttachedWindowObservation> observations;
    for (const uint64_t identity : m_attach.attachedIdentities()) {
        AttachedWindowObservation observation;
        observation.identity = identity;
        const auto windowGeom = windowGeometry(identity);
        if (windowGeom) {
            observation.windowRect =
                AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height};
            observation.minimized = windowGeom->minimized;
            observation.title = windowGeom->title;
            if (!windowGeom->minimized) {
                const DesktopPoint centre{windowGeom->x + windowGeom->width / 2.0,
                                          windowGeom->y + windowGeom->height / 2.0};
                if (const auto displayId = displayAtPoint(centre)) {
                    if (const auto display = geometryOfDisplay(*displayId)) {
                        observation.displayId = *displayId;
                        observation.display = AttachDisplayRect{display->originX, display->originY,
                                                                display->widthPoints, display->heightPoints};
                    }
                }
            }
        }
        observations.push_back(observation);
    }

    return observations;
}

// Whether the active window's rectangle changed since the last follow step
// while staying the SAME window - real motion, which the border sits out. A
// change of active window is a switch instead: the border simply jumps along
// with the region.
bool App::activeWindowMoved(const AttachDecision& decision) const
{
    if (decision.activeIdentity == 0 || decision.activeIdentity != m_activeWindowIdentity) {
        return false;
    }
    if (!m_attachLastSeenRect || !decision.activeRect) {
        return false;
    }

    return decision.activeRect->x != m_attachLastSeenRect->x || decision.activeRect->y != m_attachLastSeenRect->y ||
           decision.activeRect->width != m_attachLastSeenRect->width ||
           decision.activeRect->height != m_attachLastSeenRect->height;
}

// Follow the active window across displays: capture the one it now sits on,
// reusing the existing display-switch path.
void App::captureActiveDisplay(const AttachDecision& decision)
{
    if (decision.activeDisplayId != 0 && decision.activeDisplayId != m_captureController.capturedDisplay() &&
        m_captureController.permissionGranted() && !m_captureController.dead()) {
        m_captureController.requestDisplay(decision.activeDisplayId);
        m_captureController.start();
        m_lastActivity = glfwGetTime();
    }
}

// Applies a per-frame attach verdict - the whole focus routing in one line:
// the focused attached window's region when there is one, the global region
// otherwise. Motion detection is the follow step's business - a region
// change here may equally be the active window switching, which must not
// blank the border.
void App::applyAttachDecision(const AttachDecision& decision)
{
    setRegion(decision.region ? *decision.region : m_globalRegion);
    if (decision.closedCount > 0) {
        m_panes->showAttachNotice(decision.detachedAll ? "window closed - detached" : "window closed - still attached");
    }
    if (decision.detachedAll) {
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
}

// The event-driven side of the border's motion reaction: delivered on the
// main thread by the platform watch the moment the user grips or moves the
// active window, even mid idle-wait, so the hide precedes the first stale
// composite instead of trailing it by a poll.
void App::onWindowMotion(WindowMotionSignal signal)
{
    switch (signal) {
    case WindowMotionSignal::GripDown:
        m_attachGripActive = true;
        // A click into an attached window is often a focus change too: wake
        // the loop so the border's focus rule reacts promptly.
        glfwPostEmptyEvent();
        break;
    case WindowMotionSignal::MotionImminent:
    case WindowMotionSignal::Moved:
        m_attachRegionMovedAt = glfwGetTime();
        if (!m_attachedWindowMoving) {
            m_attachedWindowMoving = true;
            hideRegionBorder();
        }
        glfwPostEmptyEvent();
        break;
    case WindowMotionSignal::GripUp:
        m_attachGripActive = false;
        // The settle countdown starts at release, not at the last move a
        // poll happened to see.
        m_attachRegionMovedAt = glfwGetTime();
        glfwPostEmptyEvent();
        break;
    }
}

// The label prefers the window's live title - the filename in most editors
// - and follows it when the window's content changes; a face-locked window
// says so.
void App::refreshAttachedLabel(const AttachDecision& decision)
{
    if (decision.activeIdentity == 0) {
        return;
    }
    m_attachActiveLabel = borderLabelFrom(decision.activeTitle, m_attach.activeApplicationName());
    if (m_faceLock.contains(decision.activeIdentity)) {
        m_attachActiveLabel = "face - " + m_attachActiveLabel;
    }
}

// The focused window drives everything: the foreground application's
// frontmost ordinary window - frozen on the active window while its border
// is being dragged, and held while SideScopes itself is in front or no
// application is. One region kind at a time means there is no global region
// to switch to while windows are attached, so the user can work the scopes
// against the last attached region without losing it.
std::optional<uint64_t> App::resolveFocusedWindow() const
{
    if (m_attachBorderEditing && m_activeWindowIdentity != 0) {
        return m_activeWindowIdentity;
    }
    const int64_t foreground = foregroundApplicationPid();
    const uint64_t held = m_activeWindowIdentity != 0 ? m_activeWindowIdentity : m_attach.activeIdentity();
    if ((foreground == m_ownPid || foreground == 0) && held != 0) {
        // Straight after a pick the held window is the picked one - the
        // watch has not bound yet, and a window owned by a helper process
        // (a Quick Look preview) can never take the foreground for itself,
        // so this is the only thing keeping its region. A foreground of
        // zero is a focus handoff in flight - Windows reports no foreground
        // window for a frame mid-click - and must hold too: rerouting on
        // that frame wipes the held identity, so the region could never
        // survive a click into SideScopes itself.
        return held;
    }

    return focusedAttachedWindow(foreground, m_attach.attachedIdentities());
}

// One follow step: observes every attached window, lets the controller pick
// the active one and map its region, and applies the verdict. Runs twice per
// frame - once before the frame, and again right after the swap so the
// border and region are repositioned from geometry read after the vsync
// wait, not a frame earlier.
void App::followAttachedWindow()
{
    if (!m_attach.attached() || m_regionPicker.active()) {
        return;
    }

    const AttachDecision decision = m_attach.observe(gatherAttachedObservations(), resolveFocusedWindow());
    if (activeWindowMoved(decision)) {
        onWindowMotion(WindowMotionSignal::Moved);
    }
    refreshAttachedLabel(decision);
    if (decision.activeIdentity != m_activeWindowIdentity) {
        // The active window switched: the motion watch moves with it and the
        // border, no longer mid-anything, follows the routing right away.
        m_activeWindowIdentity = decision.activeIdentity;
        m_attachGripActive = false;
        m_attachedWindowMoving = false;
        unwatchWindowMotion();
        if (m_activeWindowIdentity != 0) {
            watchWindowMotion(m_activeWindowIdentity, decision.activeOwnerPid,
                              [this](WindowMotionSignal signal) { onWindowMotion(signal); });
        }
        m_faceLock.onActivated(m_activeWindowIdentity, glfwGetTime());
        m_lastActivity = glfwGetTime();
    }
    m_attachLastSeenRect = decision.activeRect;
    applyAttachDecision(decision);
    captureActiveDisplay(decision);
    SS_DIAG(Attach, "fg=%lld active=%llu display=%u region=%.1f,%.1f,%.1f,%.1f label=%s moving=%d",
            static_cast<long long>(foregroundApplicationPid()),
            static_cast<unsigned long long>(decision.activeIdentity), m_captureController.capturedDisplay(),
            m_analysis.region.leftPercent, m_analysis.region.topPercent, m_analysis.region.rightPercent,
            m_analysis.region.bottomPercent, m_attachActiveLabel.c_str(), m_attachedWindowMoving ? 1 : 0);
    const FaceLockOutcome faceLockOutcome =
        m_faceLock.update(decision, m_frameSize, m_activeWindowIdentity, m_analysis.region,
                          m_attachBorderEditing || m_attachedWindowMoving || m_attachGripActive, glfwGetTime());
    applyFaceLockOutcome(faceLockOutcome);
    if (decision.closedCount > 0) {
        m_lastActivity = glfwGetTime();
    }
    // The window has sat still long enough and nothing grips it: the border
    // may come back where the motion left it.
    if (m_attachedWindowMoving && !m_attachGripActive &&
        glfwGetTime() - m_attachRegionMovedAt > AttachMotionSettleSeconds) {
        m_attachedWindowMoving = false;
    }
    syncRegionBorder();
}

// Applies the face-lock controller's per-frame outcome to host state. An
// adopted region becomes the analysis region; a lost lock detaches its window
// and falls back to the global region - exactly what a give-up did to host
// state before. The controller has already dropped its own lock and reset its
// hunting and content watch.
void App::applyFaceLockOutcome(const FaceLockOutcome& outcome)
{
    if (outcome.applyRegion) {
        m_analysis.region = *outcome.applyRegion;
        m_analysisDirty = true;
    }
    if (outcome.lostLock) {
        const uint64_t identity = *outcome.lostLock;
        m_attach.remove(identity);
        if (m_activeWindowIdentity == identity) {
            unwatchWindowMotion();
            m_activeWindowIdentity = 0;
        }
        m_panes->showAttachNotice("face lost - region removed");
        setRegion(m_globalRegion);
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
}

// The idle tick, in slices, while windows are attached: a programmatic window
// move (a snap tool - no mouse events, no move-size loop) still takes the
// border down within a slice instead of sitting stale for the whole tick,
// and a focus change with no event of ours (Cmd+` has no app-level
// notification) still reroutes within a slice. An early wake means a real
// event arrived; the frame body handles it now.
void App::idleWaitWatchingAttachedWindow()
{
    for (int slice = 0; slice < 4; ++slice) {
        const double sliceStart = glfwGetTime();
        glfwWaitEventsTimeout(0.025);
        if (glfwGetTime() - sliceStart < 0.023) {
            return;
        }
        const auto focusedNow = focusedAttachedWindow(foregroundApplicationPid(), m_attach.attachedIdentities());
        const uint64_t focusedAttachedIdentity = focusedNow && m_attach.isAttached(*focusedNow) ? *focusedNow : 0;
        if (focusedAttachedIdentity != m_activeWindowIdentity) {
            return;
        }
        if (m_activeWindowIdentity == 0 || !m_attachLastSeenRect) {
            continue;
        }
        const auto rect = windowGeometry(m_activeWindowIdentity);
        if (!rect) {
            return;  // closed: the frame body prunes it
        }
        if (rect->x != m_attachLastSeenRect->x || rect->y != m_attachLastSeenRect->y ||
            rect->width != m_attachLastSeenRect->width || rect->height != m_attachLastSeenRect->height) {
            onWindowMotion(WindowMotionSignal::Moved);

            return;
        }
        // A face-locked region's content churn - a pan under an idle loop -
        // takes the border down within a slice, not a whole tick.
        if (m_faceLock.contains(m_activeWindowIdentity)) {
            m_faceLock.probeContentChange(m_analysis.region, m_frameSize, glfwGetTime());
            if (m_faceLock.contentUnsettled(glfwGetTime())) {
                hideRegionBorder();
            }
        }
    }
}

// Sheds only the front attached window; the last one's detach is the full
// reset back to the screen.
void App::detachActiveWindow()
{
    if (m_attach.attachedCount() > 1 && m_attach.activeIdentity() != 0) {
        m_attach.remove(m_attach.activeIdentity());
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
    } else {
        resetToFullScreen();
    }
}

/// The quick start a window click hands back: the window rectangle pulled
/// in by a fixed margin - generous enough that the border chrome and label
/// strip clear the title bar and its buttons. Toolbars are the user's
/// resize job, never a guess.
constexpr double AttachQuickStartInsetPoints = 48.0;

RegionOfInterest quickStartRegion(const RegionOfInterest& window, const DisplayGeometry& display)
{
    const double widthPoints = (window.rightPercent - window.leftPercent) / 100.0 * display.widthPoints;
    const double heightPoints = (window.bottomPercent - window.topPercent) / 100.0 * display.heightPoints;
    const double inset = std::min({AttachQuickStartInsetPoints, widthPoints / 6.0, heightPoints / 6.0});
    RegionOfInterest region = window;
    region.leftPercent += inset / display.widthPoints * 100.0;
    region.rightPercent -= inset / display.widthPoints * 100.0;
    region.topPercent += inset / display.heightPoints * 100.0;
    region.bottomPercent -= inset / display.heightPoints * 100.0;

    return region;
}

// Field diagnosis for the window-pick mapping: every rectangle in the
// chain, on the suggestions channel.
void App::logAttachMapping(const RegionPicker::WindowCandidate& picked, const RegionOfInterest& start)
{
    SS_DIAG(Suggestions, "pick window=%llu list-rect=%.1f,%.1f %.1fx%.1f suggested=%.2f,%.2f..%.2f,%.2f%%",
            static_cast<unsigned long long>(picked.identity), picked.windowRect.x, picked.windowRect.y,
            picked.windowRect.width, picked.windowRect.height, picked.region.leftPercent, picked.region.topPercent,
            picked.region.rightPercent, picked.region.bottomPercent);
    if (const auto live = windowGeometry(picked.identity)) {
        SS_DIAG(Suggestions, "pick live-rect=%.1f,%.1f %.1fx%.1f minimized=%d", live->x, live->y, live->width,
                live->height, live->minimized ? 1 : 0);
    }
    SS_DIAG(Suggestions, "pick quick-start=%.2f,%.2f..%.2f,%.2f%%", start.leftPercent, start.topPercent,
            start.rightPercent, start.bottomPercent);
}

// A confirmed region that names a window attaches to it (or re-picks an
// attached one); a rectangle drawn in attach mode binds to the frontmost
// window under it; a freehand draw sets the global region.
void App::confirmPickedRegion(const ConfirmedPick& pick)
{
    const RegionOfInterest confirmed = pick.region;
    const RegionPicker::WindowCandidate* picked = m_regionPicker.matchWindowCandidate(pick.displayId, confirmed);
    const auto geometry = geometryOfDisplay(pick.displayId);
    const auto display = geometry
                             ? std::optional<AttachDisplayRect>(AttachDisplayRect{
                                   geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints})
                             : std::nullopt;
    if (picked != nullptr && display && geometry) {
        // A window click quick-starts inset from the window's edges.
        const RegionOfInterest start = quickStartRegion(picked->region, *geometry);
        logAttachMapping(*picked, start);
        adoptAttachedPick(picked->identity, picked->ownerPid,
                          m_attach.attach(picked->identity, picked->ownerPid, picked->application, picked->windowRect,
                                          *display, start));

        return;
    }
    // A confirmed face suggestion attaches to the window under it.
    if (adoptFacePick(pick.displayId, confirmed)) {
        return;
    }
    // A rectangle drawn in attach mode binds to the frontmost window under
    // it; over no window at all it falls through to the global region.
    if (pick.attachesToWindow && display) {
        const RegionPicker::WindowCandidate* host = m_regionPicker.windowContaining(pick.displayId, confirmed);
        if (host != nullptr) {
            adoptAttachedPick(host->identity, host->ownerPid,
                              m_attach.attach(host->identity, host->ownerPid, host->application, host->windowRect,
                                              *display, confirmed));

            return;
        }
    }
    // One region kind at a time: a global draw retires every attached
    // region.
    if (m_attach.attached()) {
        m_attach.detachAll();
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
    m_globalRegion = confirmed;
    setRegion(m_globalRegion);
}

// A confirmed face suggestion becomes an attachment on the window under it:
// the window's attachment carries the region between focus changes, and the
// lock follows the face within it. A face over no suggested window falls
// through to the plain global path.
bool App::adoptFacePick(uint32_t displayId, const RegionOfInterest& confirmed)
{
    const FaceCandidate* face = m_regionPicker.matchFaceCandidate(displayId, confirmed);
    if (face == nullptr) {
        return false;
    }
    const RegionPicker::WindowCandidate* host = m_regionPicker.windowContaining(displayId, confirmed);
    const auto geometry = geometryOfDisplay(displayId);
    if (host == nullptr || !geometry) {
        return false;
    }
    const RegionOfInterest mapped = m_attach.attach(
        host->identity, host->ownerPid, host->application, host->windowRect,
        AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints},
        confirmed);
    adoptAttachedPick(host->identity, host->ownerPid, mapped);
    const FaceAnchor anchor{face->box.x + face->box.width / 2.0, face->box.y + face->box.height / 2.0,
                            static_cast<double>(face->box.width)};
    m_faceLock.addLock(host->identity,
                       face_lock::makeLock(anchor, lockRectFromPercent(confirmed, face->frameWidth, face->frameHeight)),
                       glfwGetTime(), host->windowRect);
    SS_DIAG(FaceLock, "locked to '%s' anchor=%.1f,%.1f width=%.1f", host->application.c_str(), anchor.centerX,
            anchor.centerY, anchor.width);

    return true;
}

}  // namespace sidescopes
