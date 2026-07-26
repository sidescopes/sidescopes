#include "app/frame_pacing.h"

namespace sidescopes {

FrameWaitDecision frameWaitFor(const FramePacingInputs& inputs)
{
    const bool moving = inputs.now - inputs.lastActivity <= IdleAfterSeconds;
    const bool readoutFollowing = inputs.now - inputs.lastReadoutActivity <= IdleAfterSeconds;
    const double period = moving ? ContentRedrawSeconds : ReadoutRedrawSeconds;
    const double due = inputs.lastFrameStart + period;
    const double left = due > inputs.now ? due - inputs.now : 0.0;
    if (moving || readoutFollowing) {
        return FrameWaitDecision{FrameWait::None, left};
    }
    if (inputs.attached && !inputs.pickerActive) {
        return FrameWaitDecision{FrameWait::WatchAttachedWindow, left};
    }

    return FrameWaitDecision{FrameWait::Idle, left};
}

bool outOfSight(const VisibilityInputs& inputs)
{
    if (inputs.needsFrames) {
        return false;
    }

    return inputs.sessionAsleep || inputs.applicationHidden || inputs.iconified || !inputs.windowVisible ||
           inputs.framebufferEmpty;
}

PipelineAction VisibilityGate::update(const VisibilityInputs& inputs, bool suspended, double now)
{
    if (!outOfSight(inputs)) {
        m_outOfSight = false;

        return suspended ? PipelineAction::Resume : PipelineAction::Keep;
    }
    if (!m_outOfSight) {
        m_outOfSight = true;
        m_outOfSightSince = now;
    }
    if (suspended || now - m_outOfSightSince <= OutOfSightPauseSeconds) {
        return PipelineAction::Keep;
    }

    return PipelineAction::Suspend;
}

}  // namespace sidescopes
