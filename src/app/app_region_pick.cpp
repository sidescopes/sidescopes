#include "app/app.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <mutex>
#include <optional>

#include "app/window_suggestions.h"

namespace sidescopes {

// Applies a RegionPickOutcome to host state. The picker owns its own state and
// returns only intent; every host-visible effect of a pick - the live preview,
// a pinned colour, a confirmed attachment or draw, an Esc reset, the border
// re-sync - lands here.
void App::applyRegionPickOutcome(const RegionPickOutcome& outcome)
{
    if (outcome.previewRegion) {
        const RegionOfInterest& region = *outcome.previewRegion;
        // The no-op check keeps a hover that indicates the same region from
        // nudging the worker or the activity clock every frame.
        if (region.leftPercent != m_analysis.region.leftPercent || region.topPercent != m_analysis.region.topPercent ||
            region.rightPercent != m_analysis.region.rightPercent ||
            region.bottomPercent != m_analysis.region.bottomPercent) {
            m_analysis.region = region;
            m_analysisDirty = true;
            m_lastActivity = glfwGetTime();
        }
    }
    if (outcome.pinColor) {
        m_pins.pin(*outcome.pinColor);
    }
    if (outcome.confirmed) {
        const ConfirmedPick& pick = *outcome.confirmed;
        // A pick confirmed on another display switches the capture stream to it
        // before the region is applied.
        if (pick.displayId != 0 && pick.displayId != m_captureController.capturedDisplay()) {
            m_captureController.requestDisplay(pick.displayId);
            m_captureController.start();
        }
        confirmPickedRegion(pick);
    }
    if (outcome.cancelled) {
        resetToFullScreen();
    }
    if (outcome.ended) {
        syncRegionBorder();
    }
    if (outcome.activity) {
        m_lastActivity = glfwGetTime();
    }
}

std::optional<FloatColor> App::currentScreenSampleColor() const
{
    std::lock_guard lock(m_screenSample->mutex);

    return m_screenSample->color;
}

void App::handleRegionBorderEdit()
{
    // The region border is live: dragging its edges, corners, or move tab
    // adjusts the region it currently outlines - the attached region of the
    // focused attached window, or the global one - with the scopes following.
    if (m_regionPicker.active()) {
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
        m_faceLock.clear();
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
        if (m_frameSize) {
            m_faceLock.rebindCrop(m_attachBorderEditIdentity, applied, *m_frameSize);
        }
    } else {
        m_globalRegion = edited;
    }
    m_analysis.region = applied;
    // The analysis-dirty path syncs the border this same iteration.
    m_analysisDirty = true;
    m_lastActivity = glfwGetTime();
}

// The shared tail of both attached creations: the global region retires
// (one region kind at a time), the motion state starts fresh, the watch
// rebinds on the next follow step, and the picked window comes up so the
// border never wraps someone else's pixels.
void App::adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region)
{
    m_globalRegion = RegionOfInterest{};
    // A manual pick or draw replaces whatever face lock the window wore.
    m_faceLock.removeLock(identity);
    m_attachedWindowMoving = false;
    m_attachGripActive = false;
    m_attachRegionMovedAt = -1.0;
    unwatchWindowMotion();
    m_activeWindowIdentity = 0;
    raiseWindow(identity, ownerPid);
    setRegion(region);
}

}  // namespace sidescopes
