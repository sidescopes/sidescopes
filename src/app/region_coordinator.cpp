#include "app/region_coordinator.h"

#include "app/attach_controller.h"
#include "app/border_label.h"
#include "app/capture_controller.h"
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

RegionCoordinator::RegionCoordinator(AttachController& attach, const CaptureController& capture, RegionPicker& picker,
                                     const std::optional<RegionOfInterest>& region)
    : m_attach(attach),
      m_capture(capture),
      m_picker(picker),
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
    endBorderEdit();
    RegionOutcome outcome;
    if (m_attach.attached()) {
        m_attach.detachAll();
        outcome.detachedAll = true;
    }
    m_globalRegion.reset();
    // Repeating clear does not dirty an already empty state.
    outcome.regionChanged = m_region.has_value();

    return outcome;
}

void RegionCoordinator::syncBorder(const RegionBorderState& state)
{
    // The border draws on the display the region belongs to: the one being
    // captured, or - until a session's first stream is up - the one capture
    // was asked for, so the border never waits on the stream. With neither
    // there is no display to draw on, and the platform side is left alone.
    const uint32_t display =
        m_capture.capturedDisplay() != 0 ? m_capture.capturedDisplay() : m_capture.desiredDisplay();
    if (display == 0) {
        return;
    }
    // The border shows only while this application is itself visible - a
    // hidden or minimized SideScopes must not leave regions floating on
    // screen - never during a pick or window motion, and never with no region
    // to outline. What it outlines follows the focus routing already folded
    // into the analysis region: the attached region on the focused attached
    // window with its title, else the global region with its display name. Called every
    // frame; the platform side makes the unchanged case free.
    if (m_picker.active() || !m_region || applicationHidden() || state.windowMoving || state.windowMinimized) {
        hideRegionBorder();
    } else {
        const RegionKind kind = regionKind(state.activeWindowIdentity);
        if (kind == RegionKind::Global && display != m_displayLabelId) {
            m_displayLabelId = display;
            m_displayLabel = borderLabelFrom(displayName(m_displayLabelId), "Display");
        }
        showRegionBorder(display, *m_region, kind == RegionKind::Global ? m_displayLabel : state.windowLabel, kind);
    }
}

RegionBorderEditOutcome RegionCoordinator::pollBorderEdit(uint64_t activeWindowIdentity)
{
    // The region border is live: dragging its edges, corners, or move tab
    // adjusts the region it currently outlines - the attached region of the
    // focused attached window, or the global one - with the scopes following.
    if (m_picker.active()) {
        endBorderEdit();

        return {};
    }
    const RegionBorderEdit edit = pollRegionBorderEdit();
    if (edit.closed) {
        endBorderEdit();
        return {true, false, {}};
    }
    if ((edit.editing || edit.region) && !m_borderEditing) {
        // Latch what the border showed when the drag began: no focus race
        // can reroute the edit to the other region kind. A short completed
        // gesture can arrive with geometry after its editing flag cleared.
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

    return RegionBorderEditOutcome{false, edit.bindingToggled, edit.region};
}

void RegionCoordinator::endBorderEdit()
{
    if (m_borderEditing) {
        hideAttachedEditDim();
    }
    m_borderEditing = false;
    m_borderEditIdentity = 0;
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
