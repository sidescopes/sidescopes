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

bool nothingNeedsFrames(const VisibilityInputs& inputs)
{
    if (inputs.needsFrames) {
        return false;
    }

    return inputs.sessionAsleep || inputs.applicationHidden || inputs.iconified || !inputs.windowVisible ||
           inputs.framebufferEmpty || inputs.nothingSelected;
}

PipelineAction VisibilityGate::update(const VisibilityInputs& inputs, bool suspended, double now)
{
    if (!nothingNeedsFrames(inputs)) {
        m_idle = false;

        return suspended ? PipelineAction::Resume : PipelineAction::Keep;
    }
    if (!m_idle) {
        m_idle = true;
        m_idleSince = now;
    }
    if (suspended || now - m_idleSince <= CapturePauseSeconds) {
        return PipelineAction::Keep;
    }

    return PipelineAction::Suspend;
}

}  // namespace sidescopes
