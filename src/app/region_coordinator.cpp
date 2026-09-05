#include "app/region_coordinator.h"

#include "app/attach_controller.h"
#include "app/border_label.h"
#include "app/capture_controller.h"
#include "app/face_lock_controller.h"
#include "app/region_picker.h"
#include "app/window_suggestions.h"
#include "platform/desktop.h"
#include "platform/region_selection.h"

namespace sidescopes {

RegionKind regionKind(uint64_t activeWindowIdentity)
{
    // The active identity IS the kind: the follow step takes the attached
    // region exactly while a visible attached window holds the focus, and
    // falls back to the global region the moment it does not.
    return activeWindowIdentity != 0 ? RegionKind::Attached : RegionKind::Global;
}

RegionBinding regionBinding(uint64_t activeWindowIdentity, bool faceLocked)
{
    if (activeWindowIdentity == 0) {
        return RegionBinding::Global;
    }

    return faceLocked ? RegionBinding::Face : RegionBinding::Window;
}

RegionCoordinator::RegionCoordinator(AttachController& attach, const CaptureController& capture, RegionPicker& picker,
                                     FaceLockController& faceLock, const std::optional<RegionOfInterest>& region)
    : m_attach(attach),
      m_capture(capture),
      m_picker(picker),
      m_faceLock(faceLock),
      m_region(region)
{
}

const std::optional<RegionOfInterest>& RegionCoordinator::globalRegion() const
{
    return m_globalRegion;
}

void RegionCoordinator::setGlobalRegion(const std::optional<RegionOfInterest>& region)
{
    m_globalRegion = region;
}

RegionOutcome RegionCoordinator::useRegion(const std::optional<RegionOfInterest>& region) const
{
    if (region == m_region) {
        return {};
    }
    RegionOutcome outcome;
    outcome.regionChanged = true;
    outcome.region = region;
    outcome.activity = true;

    return outcome;
}

RegionOutcome RegionCoordinator::clearRegion()
{
    // Drops all selection: a pending pick, every attached window, and the
    // global region alike. The border sync rides the analysis-dirty path.
    m_picker.cancel();
    m_faceLock.clear();
    if (m_borderEditing) {
        hideAttachedEditDim();
    }
    m_borderEditing = false;
    m_borderEditIdentity = 0;
    RegionOutcome outcome;
    if (m_attach.attached()) {
        m_attach.detachAll();
        outcome.detachedAll = true;
    }
    m_globalRegion.reset();
    // Only a clear that takes something away is a change, the same no-op test
    // useRegion makes. Escape in the empty state is the ordinary case - it is
    // also how a pick is cancelled - and reporting it as a change pushed the
    // settings, re-synced the border and re-saved the preferences for nothing.
    outcome.regionChanged = m_region.has_value();

    return outcome;
}

void RegionCoordinator::syncBorder(const RegionBorderState& state)
{
    if (m_capture.capturedDisplay() == 0) {
        return;
    }
    // The border shows only while this application is itself visible - a
    // hidden or minimized SideScopes must not leave regions floating on
    // screen - never during a pick or window motion, and never with no region
    // to outline. What it outlines follows the focus routing already folded
    // into the analysis region: the attached region on the focused attached
    // window (label and warm dress), else the plain global one. Called every
    // frame; the platform side makes the unchanged case free.
    if (m_picker.active() || !m_region || applicationHidden() || state.windowMoving || m_faceLock.hunting() ||
        m_faceLock.contentUnsettled(state.now) || state.windowMinimized) {
        hideRegionBorder();
    } else {
        const RegionBinding binding =
            regionBinding(state.activeWindowIdentity, m_faceLock.contains(state.activeWindowIdentity));
        if (binding == RegionBinding::Global && m_capture.capturedDisplay() != m_displayLabelId) {
            m_displayLabelId = m_capture.capturedDisplay();
            m_displayLabel = borderLabelFrom(displayName(m_displayLabelId), "Display");
        }
        showRegionBorder(m_capture.capturedDisplay(), *m_region,
                         binding == RegionBinding::Global ? m_displayLabel : state.windowLabel, binding);
    }
}

RegionBorderEditOutcome RegionCoordinator::pollBorderEdit(uint64_t activeWindowIdentity)
{
    // The region border is live: dragging its edges, corners, or move tab
    // adjusts the region it currently outlines - the attached region of the
    // focused attached window, or the global one - with the scopes following.
    if (m_picker.active()) {
        if (m_borderEditing) {
            hideAttachedEditDim();
        }
        m_borderEditing = false;

        return {};
    }
    const RegionBorderEdit edit = pollRegionBorderEdit();
    if (edit.editing && !m_borderEditing) {
        // Latch what the border showed when the drag began: no focus race
        // can reroute the edit to the other region kind.
        m_borderEditIdentity = activeWindowIdentity;
    }
    // While an attached border is dragged, a click-through veil dims
    // everything outside its window - the resize limit made visible.
    if (edit.editing && m_borderEditIdentity != 0 && m_borderEditIdentity == activeWindowIdentity) {
        const auto windowGeom = windowGeometry(activeWindowIdentity);
        const auto geometry = geometryOfDisplay(m_capture.capturedDisplay());
        if (windowGeom && geometry) {
            showAttachedEditDim(m_capture.capturedDisplay(), displayPercentRect(*windowGeom, *geometry));
        }
    } else if (!edit.editing && m_borderEditing) {
        hideAttachedEditDim();
    }
    m_borderEditing = edit.editing;

    return RegionBorderEditOutcome{edit.dismissed, edit.bindingToggled, edit.region};
}

bool RegionCoordinator::borderEditing() const
{
    return m_borderEditing;
}

uint64_t RegionCoordinator::borderEditIdentity() const
{
    return m_borderEditIdentity;
}

}  // namespace sidescopes
