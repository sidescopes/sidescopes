#include "app/frame_pacing.h"

namespace sidescopes {

FrameWaitDecision frameWaitFor(const FramePacingInputs& inputs)
{
    const bool moving = inputs.now - inputs.lastActivity <= IdleAfterSeconds;
    const bool readoutFollowing = inputs.now - inputs.lastReadoutActivity <= IdleAfterSeconds;
    if (!moving && !readoutFollowing) {
        if (inputs.attached && !inputs.pickerActive) {
            return FrameWaitDecision{FrameWait::WatchAttachedWindow, 0.0};
        }

        return FrameWaitDecision{FrameWait::Idle, IdleWaitSeconds};
    }

    const double due = inputs.lastFrameStart + (moving ? ContentRedrawSeconds : ReadoutRedrawSeconds);

    return FrameWaitDecision{FrameWait::UntilFramePeriod, due > inputs.now ? due - inputs.now : 0.0};
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
