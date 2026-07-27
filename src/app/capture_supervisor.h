#pragma once

#include <optional>
#include <string>

#include "app/capture_crop.h"
#include "app/frame_pacing.h"
#include "core/analysis_worker.h"
#include "core/diagnostics.h"
#include "core/frame.h"

namespace sidescopes {

/// What the capture's state is decided from, gathered by the shell once a
/// frame.
struct CaptureConditions
{
    /// Whether anything at all is asking the stream for frames.
    VisibilityInputs visibility;
    /// Whether the pipeline is already suspended, so no transition is asked
    /// for that has already happened.
    bool suspended = false;
    /// The frame the worker last saw, which is what states the display's own
    /// extent; with none there is nothing to narrow against.
    std::optional<AnalysisWorker::FrameSize> frameSize;
    /// The part of the screen the scopes read, or nothing at all.
    std::optional<RegionOfInterest> region;
    /// A face lock searches the window it is bound to, which is outside the
    /// region, so a narrowed capture would take its search away.
    bool faceLocked = false;
};

/// What the shell should do about the capture now.
struct CaptureDecision
{
    PipelineAction pipeline = PipelineAction::Keep;
    /// The line the status bar wears while the pipeline is suspended; empty
    /// unless @ref pipeline is Suspend.
    std::string pauseReason;
    /// Whether @ref crop is an answer at all. No frame has been seen yet means
    /// no opinion, which is not the same as an opinion of the whole display.
    bool cropKnown = false;
    /// What the capture should cover, or nothing for the whole display.
    std::optional<IntRect> crop;
};

/// Decides what the capture stream should be doing: running or paused, and
/// whether it delivers the whole display or only the part the region needs.
///
/// Both halves are the same question asked of the same facts once a frame -
/// how much of the screen is worth reading right now - and both were already
/// policies of their own (a visibility gate with its hysteresis, a crop
/// tracker with its settle time). What sat in the shell between them was the
/// wiring, the pause wording and the recording latch; they live here now, so
/// the shell gathers the conditions and carries the answer out.
class CaptureSupervisor
{
public:
    /// @return What to do now, from @p conditions at @p now.
    [[nodiscard]] CaptureDecision update(const CaptureConditions& conditions, double now);

private:
    /// The narrowing half: what the capture should cover, and the recording
    /// line when that changes.
    void decideCrop(const CaptureConditions& conditions, double now, CaptureDecision& decision);

    VisibilityGate m_visibility;
    CropTracker m_crop;
    /// The crop this recording has been told about.
    DiagOnChange<std::optional<IntRect>> m_loggedCrop{DiagChannel::Perf};
};

}  // namespace sidescopes
