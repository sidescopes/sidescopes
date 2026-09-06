#include "app/region_motion.h"

namespace sidescopes {

RegionMotionStep RegionMotionTracker::update(const RegionMotionInputs& inputs)
{
    // A region change while a window carries it is not a drag: the user's hand
    // is on the window, not on the region. Recording it as one would leave a
    // drag standing behind a window that has just landed, so the sharp trace
    // would be preceded by a coarse one nobody asked for.
    if (inputs.regionChanged && !inputs.windowMoving) {
        m_draggedAt = inputs.now;
    }

    const RegionMotion was = m_motion;
    if (inputs.windowMoving) {
        m_motion = RegionMotion::Carried;
    } else if (m_draggedAt && inputs.now - *m_draggedAt < RegionSettleSeconds) {
        m_motion = RegionMotion::Dragged;
    } else {
        m_motion = RegionMotion::Still;
    }
    return RegionMotionStep{m_motion, m_motion == RegionMotion::Carried, m_motion != was};
}

}  // namespace sidescopes
