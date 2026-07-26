#include "app/region_motion.h"

namespace sidescopes {

RegionMotionStep RegionMotionTracker::update(const RegionMotionInputs& inputs)
{
    // The speed is read before the timestamp moves, since it is the gap since
    // the last change that it is measured over.
    const bool thrown = throwing(inputs);
    // A region change while a window carries it is not a drag: the user's hand
    // is on the window, not on the region. Recording it as one would leave a
    // drag standing behind a window that has just landed, so the sharp trace
    // would be preceded by a coarse one nobody asked for.
    if (inputs.regionChanged && !inputs.windowMoving) {
        m_draggedAt = inputs.now;
    }

    const RegionMotion was = m_motion;
    const bool wasThrown = m_thrown;
    if (inputs.windowMoving) {
        m_motion = RegionMotion::Carried;
    } else if (m_draggedAt && inputs.now - *m_draggedAt < RegionSettleSeconds) {
        m_motion = RegionMotion::Dragged;
    } else {
        m_motion = RegionMotion::Still;
    }
    // A region nobody is dragging is not being thrown either, so the throw ends
    // with the gesture rather than surviving into the next one.
    m_thrown = m_motion == RegionMotion::Dragged && thrown;

    return RegionMotionStep{m_motion, m_thrown, m_motion != was || m_thrown != wasThrown};
}

bool RegionMotionTracker::throwing(const RegionMotionInputs& inputs)
{
    if (inputs.windowMoving) {
        return false;
    }
    // A drag arrives as a burst of changes with gaps between them, and the
    // gaps are not the hand stopping. Only a change carries new evidence about
    // its speed, so between them the verdict stands.
    if (!inputs.regionChanged || !m_draggedAt) {
        return m_thrown;
    }
    const double since = inputs.now - *m_draggedAt;
    if (since <= 0.0) {
        // Two changes within one reading of the clock say nothing about speed.
        return m_thrown;
    }
    if (since > RegionSettleSeconds) {
        // The last change belongs to a gesture that has already ended, so this
        // one is the first of a new one and there is nothing to measure a speed
        // over. It matters for the picker: hovering from one window candidate
        // to another moves the previewed region a third of the screen in a
        // single step, and reading that as a throw would hold the very preview
        // the user is hovering to see.
        return false;
    }

    const double speed = inputs.travelPercent / since;

    return m_thrown ? speed > ScannedSpeedPercent : speed > ThrownSpeedPercent;
}

}  // namespace sidescopes
