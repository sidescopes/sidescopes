#include "app/region_coordinator.h"

#include <utility>

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
                                     FaceLockController& faceLock, const std::optional<RegionOfInterest>& region,
                                     std::function<double()> clock)
    : m_attach(attach),
      m_capture(capture),
      m_picker(picker),
      m_faceLock(faceLock),
      m_region(region),
      m_clock(std::move(clock))
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
    m_borderMotion.reset();
    m_presentedRegion.reset();
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
    if (m_capture.capturedDisplay() == 0) {
        m_borderMotion.reset();
        m_presentedRegion.reset();
        return;
    }
    // The border shows only while this application is itself visible - a
    // hidden or minimized SideScopes must not leave regions floating on
    // screen - never during a pick or window motion, and never with no region
    // to outline. What it outlines follows the focus routing already folded
    // into the analysis region: the attached region on the focused attached
    // window (label and warm dress), else the plain global one. Called every
    // frame; the platform side makes the unchanged case free.
    if (m_picker.active() || !m_region || applicationHidden() || state.windowMoving || state.windowMinimized) {
        m_borderMotion.reset();
        m_presentedRegion.reset();
        hideRegionBorder();
    } else {
        const RegionBinding binding =
            regionBinding(state.activeWindowIdentity, m_faceLock.contains(state.activeWindowIdentity));
        if (binding == RegionBinding::Global && m_capture.capturedDisplay() != m_displayLabelId) {
            m_displayLabelId = m_capture.capturedDisplay();
            m_displayLabel = borderLabelFrom(displayName(m_displayLabelId), "Display");
        }
        const auto context = std::tuple(m_capture.capturedDisplay(), m_capture.streamEpoch(),
                                        state.activeWindowIdentity, m_faceLock.selectionRevision(), binding);
        const bool animate = binding == RegionBinding::Face && !m_borderEditing && context == m_motionContext;
        m_motionContext = context;
        const auto presented = m_borderMotion.update(*m_region, m_clock(), animate);
        m_presentedRegion = presented;
        showRegionBorder(m_capture.capturedDisplay(), presented,
                         binding == RegionBinding::Global ? m_displayLabel : state.windowLabel, binding);
    }
}

bool RegionCoordinator::borderAnimating() const
{
    return m_borderMotion.active();
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
    RegionBorderEdit edit = pollRegionBorderEdit();
    if (edit.closed) {
        endBorderEdit();
        m_borderMotion.reset();
        return {true, false, {}};
    }
    if ((edit.editing || edit.region) && !m_borderEditing) {
        // Latch what the border showed when the drag began: no focus race
        // can reroute the edit to the other region kind. A short completed
        // gesture can arrive with geometry after its editing flag cleared.
        m_borderEditIdentity = activeWindowIdentity;
        // The native grab starts at the visible rectangle, which may still
        // be approaching the detected crop. Adopt exactly that selection
        // before tracking or another presentation tick can move it again.
        if (!edit.region && m_borderMotion.active()) {
            edit.region = m_presentedRegion;
        }
        m_borderMotion.reset();
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
