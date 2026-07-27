#include <catch2/catch_test_macros.hpp>

#include "app/capture_supervisor.h"

namespace sidescopes {
namespace {

// A window showing the scopes over a selected region, with a frame in hand.
CaptureConditions watching()
{
    CaptureConditions conditions;
    conditions.visibility.windowVisible = true;
    conditions.frameSize = AnalysisWorker::FrameSize{1920, 1080, 1920, 1080};
    conditions.region = RegionOfInterest{25.0, 25.0, 75.0, 75.0};

    return conditions;
}

}  // namespace

TEST_CASE("A window out of sight and an empty selection pause for different reasons")
{
    CaptureSupervisor supervisor;
    CaptureConditions hidden = watching();
    hidden.visibility.applicationHidden = true;
    // The hysteresis has to run out before anything is asked for.
    REQUIRE(supervisor.update(hidden, 0.0).pipeline == PipelineAction::Keep);
    const CaptureDecision paused = supervisor.update(hidden, 10.0);
    REQUIRE(paused.pipeline == PipelineAction::Suspend);
    CHECK(paused.pauseReason == "paused - the window is out of sight");

    CaptureSupervisor idle;
    CaptureConditions empty = watching();
    empty.visibility.nothingSelected = true;
    empty.region.reset();
    REQUIRE(idle.update(empty, 0.0).pipeline == PipelineAction::Keep);
    const CaptureDecision noRegion = idle.update(empty, 10.0);
    REQUIRE(noRegion.pipeline == PipelineAction::Suspend);
    CHECK(noRegion.pauseReason == "paused - no region selected");
}

TEST_CASE("A running pipeline is left alone, and a suspended one resumes")
{
    CaptureSupervisor supervisor;
    const CaptureDecision running = supervisor.update(watching(), 0.0);
    CHECK(running.pipeline == PipelineAction::Keep);
    CHECK(running.pauseReason.empty());

    CaptureConditions resumable = watching();
    resumable.suspended = true;
    CHECK(supervisor.update(resumable, 1.0).pipeline == PipelineAction::Resume);
}

TEST_CASE("Nothing is narrowed until a frame has said how large the display is")
{
    CaptureSupervisor supervisor;
    CaptureConditions blind = watching();
    blind.frameSize.reset();
    const CaptureDecision decision = supervisor.update(blind, 0.0);

    // No opinion at all, which is not the same as an opinion of the whole
    // display: the capture keeps whatever it is already covering.
    CHECK_FALSE(decision.cropKnown);
    CHECK_FALSE(decision.crop);
}

TEST_CASE("The supervisor narrows a settled region to itself")
{
    CaptureSupervisor supervisor;
    const CaptureConditions conditions = watching();
    // The crop settles before it is asked for, so the first answer is the whole
    // display and a later one is the region.
    REQUIRE(supervisor.update(conditions, 0.0).cropKnown);
    const CaptureDecision settled = supervisor.update(conditions, 10.0);
    REQUIRE(settled.cropKnown);
    REQUIRE(settled.crop);
    CHECK(settled.crop->width < 1920);
    CHECK(settled.crop->height < 1080);
}

TEST_CASE("A face lock and another frame reader both keep the whole display")
{
    CaptureSupervisor locked;
    CaptureConditions faceLocked = watching();
    faceLocked.faceLocked = true;
    REQUIRE(locked.update(faceLocked, 0.0).cropKnown);
    CHECK_FALSE(locked.update(faceLocked, 10.0).crop);

    CaptureSupervisor reading;
    CaptureConditions picking = watching();
    picking.visibility.needsFrames = true;
    REQUIRE(reading.update(picking, 0.0).cropKnown);
    CHECK_FALSE(reading.update(picking, 10.0).crop);
}

TEST_CASE("With no region the capture covers the whole display")
{
    CaptureSupervisor supervisor;
    CaptureConditions empty = watching();
    empty.region.reset();
    REQUIRE(supervisor.update(empty, 0.0).cropKnown);

    // The colour under the pointer is read from the stream wherever it lands,
    // so an empty session is exactly when narrowing would cost the most.
    CHECK_FALSE(supervisor.update(empty, 10.0).crop);
}

}  // namespace sidescopes
