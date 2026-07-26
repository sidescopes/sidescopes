#include "app/frame_pacing.h"

namespace sidescopes {

FrameWaitDecision frameWaitFor(const FramePacingInputs& inputs)
{
    const bool moving = inputs.now - inputs.lastActivity <= IdleAfterSeconds;
    const bool following = inputs.now - inputs.lastReadoutActivity <= IdleAfterSeconds ||
                           inputs.now - inputs.lastPointerMove <= IdleAfterSeconds;
    const double period = moving ? ContentRedrawSeconds : ReadoutRedrawSeconds;
    const double due = inputs.lastFrameStart + period;
    const double left = due > inputs.now ? due - inputs.now : 0.0;
    if (moving || following) {
        return FrameWaitDecision{FrameWait::None, left};
    }
    if (inputs.attached && !inputs.pickerActive) {
        return FrameWaitDecision{FrameWait::WatchAttachedWindow, left};
    }

    return FrameWaitDecision{FrameWait::Idle, left};
}

bool frameWorthDrawing(const RedrawInputs& inputs)
{
    // The picture is already different from the one on screen.
    const bool changed = inputs.outputPending || inputs.framebufferChanged || inputs.statusChanged;
    // The interface has not finished with the last thing that happened. Hover
    // highlights, tooltip delays and carets all advance on drawn frames, so an
    // interaction owes frames for a while after its last event.
    // A pointer still moving is in the same list: it carries the readings, and
    // it goes on owing frames for a moment after it stops because a marker
    // cannot set off until its next sample lands.
    const bool settling = inputs.textInputActive || inputs.overlayActive ||
                          inputs.now - inputs.lastInputEvent <= InputSettleSeconds ||
                          inputs.now - inputs.lastActivity <= IdleAfterSeconds ||
                          inputs.now - inputs.lastReadoutActivity <= IdleAfterSeconds ||
                          inputs.now - inputs.lastPointerMove <= IdleAfterSeconds;
    // Something timed has left the picture and no frame has taken it away.
    // Measured against the last frame drawn rather than cleared by the thing
    // that timed out, so one frame settles it and the next asks for nothing -
    // and a deadline of zero, meaning none was ever set, can never be earlier
    // than a frame that has been drawn.
    const bool expired = inputs.now >= inputs.redrawDue && inputs.lastDrawn < inputs.redrawDue;

    return changed || settling || expired;
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
