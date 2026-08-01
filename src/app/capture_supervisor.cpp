#include "app/capture_supervisor.h"

namespace sidescopes {

CaptureDecision CaptureSupervisor::update(const CaptureConditions& conditions, double now)
{
    CaptureDecision decision;
    // Which reason to name in the status line, read back off the inputs rather
    // than gathered a second time: an empty selection is the only one of them
    // that pauses a window the user is looking at.
    const VisibilityInputs& sight = conditions.visibility;
    const bool outOfSight = sight.sessionAsleep || sight.applicationHidden || sight.iconified || !sight.windowVisible ||
                            sight.framebufferEmpty;

    decision.pipeline = m_visibility.update(sight, conditions.suspended, now);
    switch (decision.pipeline) {
    case PipelineAction::Suspend:
        // Named input by input, not merely "out of sight". Which one stopped
        // the pipeline is the whole question when a reading goes stale, and a
        // recording that answers it with a category cannot be argued with.
        SS_DIAG(Perf, "pipeline suspended - asleep=%d hidden=%d iconified=%d invisible=%d nofb=%d noregion=%d probe=%d",
                static_cast<int>(sight.sessionAsleep), static_cast<int>(sight.applicationHidden),
                static_cast<int>(sight.iconified), static_cast<int>(!sight.windowVisible),
                static_cast<int>(sight.framebufferEmpty), static_cast<int>(sight.nothingSelected),
                static_cast<int>(sight.probeNeedsFrames));
        decision.pauseReason = outOfSight ? "paused - the window is out of sight" : "paused - no region selected";
        break;
    case PipelineAction::Resume:
        SS_DIAG(Perf, "pipeline resumed");
        break;
    case PipelineAction::Keep:
        break;
    }
    decideCrop(conditions, now, decision);

    return decision;
}

// Asks the capture for only the pixels the region needs. The compositor renders,
// scales and delivers whatever the stream is configured for, so a region a
// fraction of the display was still costing a whole display's worth of that work
// every frame, plus the copy into the mailbox.
void CaptureSupervisor::decideCrop(const CaptureConditions& conditions, double now, CaptureDecision& decision)
{
    if (!conditions.frameSize) {
        return;
    }
    const int displayWidth = conditions.frameSize->displayWidth;
    const int displayHeight = conditions.frameSize->displayHeight;
    // No region narrows nothing: the colour under the pointer is read from the
    // stream wherever it lands, and a narrowed stream would leave that reading
    // to the far slower whole-screen sample for the whole of an empty session.
    const IntRect regionPixels = conditions.region ? conditions.region->toPixels(displayWidth, displayHeight)
                                                   : IntRect{0, 0, displayWidth, displayHeight};
    decision.cropKnown = true;
    decision.crop = m_crop.decide(regionPixels, displayWidth, displayHeight, conditions.visibility.needsFrames,
                                  conditions.faceLocked, now);
    if (m_loggedCrop.shouldLog(decision.crop)) {
        if (decision.crop) {
            SS_DIAG(Perf, "capture narrowed to %dx%d at %d,%d", decision.crop->width, decision.crop->height,
                    decision.crop->x, decision.crop->y);
        } else {
            SS_DIAG(Perf, "capture covers the whole display");
        }
    }
}

}  // namespace sidescopes
