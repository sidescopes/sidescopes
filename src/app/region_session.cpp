#include "app/region_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <utility>

#include "app/border_label.h"
#include "app/capture_controller.h"
#include "app/face_lock.h"
#include "app/region_geometry.h"
#include "app/window_suggestions.h"
#include "core/diagnostics.h"
#include "platform/desktop.h"

extern "C" double glfwGetTime(void);
extern "C" void glfwPostEmptyEvent(void);
extern "C" void glfwWaitEventsTimeout(double timeout);

namespace sidescopes {
namespace {

// The border returns this long after the active window last moved and the
// grip released - what keeps a slow drag's sparse updates from flickering it.
constexpr double AttachMotionSettleSeconds = 0.2;

// The region for a diagnostics line. "none" rather than a rectangle when
// nothing is selected: the whole-display percentages a default would print
// read as a real selection, which is the sort of lie a log costs hours over.
std::string regionDiagText(const std::optional<RegionOfInterest>& region)
{
    if (!region) {
        return "none";
    }
    char text[48];
    std::snprintf(text, sizeof(text), "%.1f,%.1f,%.1f,%.1f", region->leftPercent, region->topPercent,
                  region->rightPercent, region->bottomPercent);

    return text;
}

}  // namespace

// Gathers this frame's observation for every attached window: geometry,
// minimized state, and - for visible ones - the display it sits on.
std::vector<AttachedWindowObservation> RegionSession::gatherAttachedObservations() const
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
bool RegionSession::activeWindowMoved(const AttachDecision& decision) const
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
void RegionSession::captureActiveDisplay(const AttachDecision& decision)
{
    if (decision.activeDisplayId != 0 && decision.activeDisplayId != m_capture.capturedDisplay() &&
        m_capture.permissionGranted() && !m_capture.dead()) {
        m_capture.requestDisplay(decision.activeDisplayId);
        m_capture.start();
        m_pending.activity = true;
    }
}

// Applies a per-frame attach verdict - the whole focus routing in one line:
// the focused attached window's region when there is one, the global region
// otherwise. Motion detection is the follow step's business - a region
// change here may equally be the active window switching, which must not
// blank the border.
void RegionSession::applyAttachDecision(const AttachDecision& decision)
{
    applyRegionOutcome(m_regions.useRegion(decision.region ? decision.region : m_regions.globalRegion()));
    if (decision.closedCount > 0) {
        setStatus(decision.detachedAll ? "window closed - detached" : "window closed - still attached");
    }
    if (decision.detachedAll) {
        releaseActiveWindow();
    }
}

// The event-driven side of the border's motion reaction: delivered on the
// main thread by the platform watch the moment the user grips or moves the
// active window, even mid idle-wait, so the hide precedes the first stale
// composite instead of trailing it by a poll.
void RegionSession::onWindowMotion(WindowMotionSignal signal)
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

// The label prefers the window's live title - the filename in most editors -
// and follows it when the window's content changes. Binding state belongs to
// the adjacent icon, never in a prefix that could be mistaken for the title.
void RegionSession::refreshAttachedLabel(const AttachDecision& decision)
{
    if (decision.activeIdentity == 0) {
        return;
    }
    m_attachActiveLabel = borderLabelFrom(decision.activeTitle, m_attach.activeApplicationName());
}

// The focused window drives everything: the foreground application's
// frontmost ordinary window - frozen on the active window while its border
// is being dragged, and held while SideScopes itself is in front or no
// application is. One region kind at a time means there is no global region
// to switch to while windows are attached, so the user can work the scopes
// against the last attached region without losing it.
std::optional<uint64_t> RegionSession::resolveFocusedWindow() const
{
    if (m_regions.borderEditing() && m_activeWindowIdentity != 0) {
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
void RegionSession::followAttachedWindow()
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
        m_pending.activity = true;
    }
    m_attachLastSeenRect = decision.activeRect;
    applyAttachDecision(decision);
    captureActiveDisplay(decision);
    SS_DIAG(Attach, "fg=%lld active=%llu display=%u region=%s label=%s moving=%d",
            static_cast<long long>(foregroundApplicationPid()),
            static_cast<unsigned long long>(decision.activeIdentity), m_capture.capturedDisplay(),
            regionDiagText(m_region).c_str(), m_attachActiveLabel.c_str(), m_attachedWindowMoving ? 1 : 0);
    const FaceLockOutcome faceLockOutcome =
        m_faceLock.update(decision, m_frameSize, m_activeWindowIdentity, m_region,
                          m_regions.borderEditing() || m_attachedWindowMoving || m_attachGripActive, glfwGetTime());
    applyFaceLockOutcome(faceLockOutcome);
    if (decision.closedCount > 0) {
        m_pending.activity = true;
    }
    // The window has sat still long enough and nothing grips it: the border
    // may come back where the motion left it.
    if (m_attachedWindowMoving && !m_attachGripActive &&
        glfwGetTime() - m_attachRegionMovedAt > AttachMotionSettleSeconds) {
        m_attachedWindowMoving = false;
    }
    m_regions.syncBorder(borderState());
}

// Applies the face-lock controller's per-frame outcome to host state. An
// adopted region becomes the analysis region; a lost lock detaches its window
// and falls back to the global region - exactly what a give-up did to host
// state before. The controller has already dropped its own lock and reset its
// hunting and content watch.
void RegionSession::applyFaceLockOutcome(const FaceLockOutcome& outcome)
{
    if (outcome.applyRegion) {
        m_region = *outcome.applyRegion;
        m_pending.regionChanged = true;
    }
    if (outcome.lostLock) {
        const uint64_t identity = *outcome.lostLock;
        m_attach.remove(identity);
        if (m_activeWindowIdentity == identity) {
            releaseActiveWindow();
        }
        setStatus("face lost - region removed");
        applyRegionOutcome(m_regions.useRegion(m_regions.globalRegion()));
        m_regions.syncBorder(borderState());
        m_pending.activity = true;
    }
}

// The idle tick, in slices, while windows are attached: a programmatic window
// move (a snap tool - no mouse events, no move-size loop) still takes the
// border down within a slice instead of sitting stale for the whole tick,
// and a focus change with no event of ours (Cmd+` has no app-level
// notification) still reroutes within a slice. An early wake means a real
// event arrived; the frame body handles it now.
void RegionSession::idleWaitWatchingAttachedWindow()
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
            m_faceLock.probeContentChange(m_region, glfwGetTime());
            if (m_faceLock.contentUnsettled(glfwGetTime())) {
                hideRegionBorder();
            }
        }
    }
}

// Sheds only the front attached window; the last one's detach clears the
// selection outright.
void RegionSession::detachActiveWindow()
{
    if (m_attach.attachedCount() > 1 && m_attach.activeIdentity() != 0) {
        m_faceLock.removeLock(m_attach.activeIdentity());
        m_attach.remove(m_attach.activeIdentity());
        releaseActiveWindow();
    } else {
        applyRegionOutcome(m_regions.clearRegion());
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
void RegionSession::logAttachMapping(const RegionPicker::WindowCandidate& picked, const RegionOfInterest& start)
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
void RegionSession::confirmPickedRegion(const ConfirmedPick& pick)
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
        m_faceLock.clear();
        releaseActiveWindow();
    }
    m_regions.setGlobalRegion(confirmed);
    applyRegionOutcome(m_regions.useRegion(confirmed));
}

// A confirmed face suggestion becomes an attachment on the window under it:
// the window's attachment carries the region between focus changes, and the
// lock follows the face within it. A face over no suggested window falls
// through to the plain global path.
bool RegionSession::adoptFacePick(uint32_t displayId, const RegionOfInterest& confirmed)
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

// Applies a RegionPickOutcome to host state. The picker owns its own state and
// returns only intent; every host-visible effect of a pick - the live preview,
// a pinned colour, a confirmed attachment or draw, an Esc clear, the border
// re-sync - lands here.
void RegionSession::applyRegionPickOutcome(const RegionPickOutcome& outcome)
{
    if (outcome.previewRegion) {
        // The coordinator's no-op check keeps a hover that indicates the same
        // region from nudging the worker or the activity clock every frame.
        applyRegionOutcome(m_regions.useRegion(outcome.previewRegion));
    }
    if (outcome.pinColor) {
        m_pending.pinColor = outcome.pinColor;
    }
    if (outcome.confirmed) {
        const ConfirmedPick& pick = *outcome.confirmed;
        // A pick confirmed on another display switches the capture stream to it
        // before the region is applied.
        if (pick.displayId != 0 && pick.displayId != m_capture.capturedDisplay()) {
            m_capture.requestDisplay(pick.displayId);
            m_capture.start();
        }
        confirmPickedRegion(pick);
    }
    if (outcome.cancelled) {
        applyRegionOutcome(m_regions.clearRegion());
    }
    if (outcome.ended) {
        m_regions.syncBorder(borderState());
    }
    if (outcome.activity) {
        m_pending.activity = true;
    }
}

// Carries out what the border's live edit decided. The coordinator has
// already latched which region the drag began on and dressed the screen for
// it; what is left is the region work only the host can do.
void RegionSession::applyBorderEditOutcome(const RegionBorderEditOutcome& outcome)
{
    if (outcome.dismissed) {
        dismissEditedBorder();
    } else if (outcome.bindingToggled) {
        toggleRegionBinding();
    } else if (outcome.edited) {
        applyBorderEdit(*outcome.edited);
    }
}

// The border's binding control progressively loosens what the region follows.
// A face-tracked region first freezes at its CURRENT rectangle inside the
// window; a second click lets go of the window and makes it global. A global
// region attaches to the frontmost window under it. Explicit conversions only
// - the structural no-conversion rule is about drags and focus races, never
// this button.
void RegionSession::toggleRegionBinding()
{
    const RegionBinding binding = regionBinding(m_activeWindowIdentity, m_faceLock.contains(m_activeWindowIdentity));
    if (binding == RegionBinding::Face) {
        const auto geometry = geometryOfDisplay(m_capture.capturedDisplay());
        const auto windowGeom = windowGeometry(m_activeWindowIdentity);
        if (m_region && geometry && windowGeom) {
            // The attach controller still holds the rectangle from the face's
            // original pick. Re-anchor it to the face's current position
            // before dropping the lock, or the next follow step would snap
            // back to that original rectangle.
            const RegionOfInterest frozen = m_attach.editRegion(
                *m_region, AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height},
                AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints});
            m_faceLock.removeLock(m_activeWindowIdentity);
            applyRegionOutcome(m_regions.useRegion(frozen));
        }
    } else if (binding == RegionBinding::Window) {
        const std::optional<RegionOfInterest> region = m_region;
        m_attach.detachAll();
        m_faceLock.clear();
        releaseActiveWindow();
        m_regions.setGlobalRegion(region);
        applyRegionOutcome(m_regions.useRegion(region));
    } else {
        attachGlobalRegionToWindow();
    }
    m_pending.activity = true;
    m_regions.syncBorder(borderState());
}

// Attaching a global region: the frontmost on-screen window under the
// region's centre becomes its window, the rectangle staying exactly where
// it is. Over no window at all this is a no-op - predictable beats
// guessing a target.
void RegionSession::attachGlobalRegionToWindow()
{
    const uint32_t displayId = m_capture.capturedDisplay();
    const auto geometry = geometryOfDisplay(displayId);
    if (!geometry || !m_regions.globalRegion()) {
        return;
    }
    const RegionOfInterest region = *m_regions.globalRegion();
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
// attached one detaches from its window only, the global one goes away
// entirely; the other attached windows keep their regions either way.
void RegionSession::dismissEditedBorder()
{
    if (regionKind(m_activeWindowIdentity) == RegionKind::Attached) {
        m_faceLock.removeLock(m_activeWindowIdentity);
        m_attach.remove(m_activeWindowIdentity);
        releaseActiveWindow();
    } else {
        m_regions.setGlobalRegion(std::nullopt);
    }
    applyRegionOutcome(m_regions.useRegion(m_regions.globalRegion()));
    m_pending.activity = true;
}

// Routes a border drag to the region kind it began on. An edit that began
// on an attached border may NEVER fall through to the global region - no
// accidental conversion; if the attached routing cannot resolve, the edit
// is dropped instead.
void RegionSession::applyBorderEdit(const RegionOfInterest& edited)
{
    RegionOfInterest applied = edited;
    if (m_regions.borderEditIdentity() != 0) {
        if (m_regions.borderEditIdentity() != m_activeWindowIdentity) {
            return;
        }
        const auto geometry = geometryOfDisplay(m_capture.capturedDisplay());
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
        if (m_frameSize) {
            m_faceLock.rebindCrop(m_regions.borderEditIdentity(), applied, *m_frameSize);
        }
    } else {
        m_regions.setGlobalRegion(edited);
    }
    m_region = applied;
    // The analysis-dirty path syncs the border this same iteration.
    m_pending.regionChanged = true;
    m_pending.activity = true;
}

// The shared tail of both attached creations: the global region retires
// (one region kind at a time), the motion state starts fresh, the watch
// rebinds on the next follow step, and the picked window comes up so the
// border never wraps someone else's pixels.
void RegionSession::adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region)
{
    m_regions.setGlobalRegion(std::nullopt);
    // A manual pick or draw replaces whatever face lock the window wore.
    m_faceLock.removeLock(identity);
    releaseActiveWindow();
    raiseWindow(identity, ownerPid);
    applyRegionOutcome(m_regions.useRegion(region));
}

RegionBorderState RegionSession::borderState() const
{
    return RegionBorderState{m_attachActiveLabel, m_activeWindowIdentity, m_attachedWindowMoving, m_windowMinimized,
                             glfwGetTime()};
}

void RegionSession::releaseActiveWindow()
{
    unwatchWindowMotion();
    m_activeWindowIdentity = 0;
    m_attachedWindowMoving = false;
    m_attachGripActive = false;
    m_attachRegionMovedAt = -1.0;
    m_attachLastSeenRect.reset();
    m_attachActiveLabel.clear();
}

void RegionSession::applyRegionOutcome(const RegionOutcome& outcome)
{
    if (outcome.detachedAll) {
        releaseActiveWindow();
    }
    if (outcome.regionChanged) {
        m_region = outcome.region;
        m_pending.regionChanged = true;
    }
    if (outcome.activity) {
        m_pending.activity = true;
    }
}

RegionSession::RegionSession(CaptureController& capture, AnalysisWorker& worker, ScreenCaptureSource& source)
    : m_capture(capture),
      m_faceLock(m_attach, worker, capture),
      m_regionPicker(capture, worker, source),
      m_regions(m_attach, capture, m_regionPicker, m_faceLock, m_region),
      m_ownPid(ownApplicationPid())
{
}

RegionSession::~RegionSession()
{
    shutdown();
}

void RegionSession::shutdown()
{
    if (m_stopped) {
        return;
    }
    m_stopped = true;
    unwatchWindowMotion();
    if (m_regionPicker.active()) {
        m_regionPicker.cancel();
    }
    while (backgroundWorkRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

RegionPicker& RegionSession::picker()
{
    return m_regionPicker;
}

const AttachController& RegionSession::attachments() const
{
    return m_attach;
}

bool RegionSession::interacting() const
{
    return m_regionPicker.active() || m_regions.borderEditing();
}

bool RegionSession::carried() const
{
    return m_attachedWindowMoving && !m_regionPicker.active();
}

bool RegionSession::faceLocked() const
{
    return m_faceLock.locked();
}

bool RegionSession::backgroundWorkRunning() const
{
    return m_faceLock.probeRunning() || m_regionPicker.scansRunning();
}

RegionSessionOutcome RegionSession::takeOutcome()
{
    m_pending.region = m_region;
    return std::exchange(m_pending, {});
}

void RegionSession::setStatus(std::string message)
{
    m_pending.status = std::move(message);
}

RegionSessionOutcome RegionSession::initializeGlobalRegion(const RegionOfInterest& region)
{
    m_regions.setGlobalRegion(region);
    applyRegionOutcome(m_regions.useRegion(region));
    return takeOutcome();
}

RegionSessionOutcome RegionSession::follow(bool windowMinimized, std::optional<AnalysisWorker::FrameSize> frameSize)
{
    m_windowMinimized = windowMinimized;
    m_frameSize = frameSize;
    followAttachedWindow();
    return takeOutcome();
}

RegionSessionOutcome RegionSession::poll(bool windowMinimized, std::optional<AnalysisWorker::FrameSize> frameSize,
                                         std::optional<FloatColor> screenSampleColor)
{
    m_windowMinimized = windowMinimized;
    m_frameSize = frameSize;
    applyRegionPickOutcome(m_regionPicker.openIfRequested(m_region.has_value()));
    applyBorderEditOutcome(m_regions.pollBorderEdit(m_activeWindowIdentity));
    applyRegionPickOutcome(m_regionPicker.poll(frameSize, screenSampleColor));
    return takeOutcome();
}

RegionSessionOutcome RegionSession::clear()
{
    applyRegionOutcome(m_regions.clearRegion());
    return takeOutcome();
}

RegionSessionOutcome RegionSession::dismiss()
{
    dismissEditedBorder();
    return takeOutcome();
}

RegionSessionOutcome RegionSession::detach()
{
    detachActiveWindow();
    return takeOutcome();
}

void RegionSession::syncBorder(bool windowMinimized)
{
    m_windowMinimized = windowMinimized;
    m_regions.syncBorder(borderState());
}

}  // namespace sidescopes
