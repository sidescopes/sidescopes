#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include "app/attach_controller.h"
#include "app/capture_controller.h"
#include "app/face_lock_controller.h"
#include "app/region_coordinator.h"
#include "app/region_picker.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/frame_mailbox.h"
#include "core/region_kind.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "region_overlay_stubs.h"
#include "test_frame.h"

namespace sidescopes {
namespace {

using Catch::Matchers::WithinAbs;
using test::desktopStubs;
using test::FakeCaptureSource;
using test::makeTarget;
using test::regionOverlayStubs;

constexpr uint32_t StreamedDisplay = 5;

// The coordinator plus the whole shell it reads: the region it is handed is
// the one the scopes are reading right now, which the host owns and it only
// observes.
struct CoordinatorFixture
{
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    AttachController attach;
    RegionPicker picker{capture, worker, source};
    FaceLockController faceLock{attach, worker, capture};
    RegionOfInterest region;
    RegionCoordinator coordinator{attach, capture, picker, faceLock, region};

    CoordinatorFixture()
    {
        desktopStubs().reset();
        regionOverlayStubs().reset();
        source.targets = {makeTarget(StreamedDisplay, "Test display")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(StreamedDisplay);
        REQUIRE(capture.start());
    }

    // A border sync with everything quiet, so each test turns on only the one
    // condition it is about.
    void sync(const std::string& attachedLabel = "", uint64_t activeIdentity = 0)
    {
        coordinator.syncBorder(RegionBorderState{attachedLabel, activeIdentity, false, false, 0.0});
    }

    // Runs a pan under the face lock's content watch: two frames far enough
    // apart that the region is unsettled as of t = 0.
    void unsettleContent()
    {
        worker.start();
        const AnalysisWorker::FrameSize frameSize{16, 16};
        for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
            const Color shade = sequence == 1 ? Color{0, 0, 0} : Color{255, 255, 255};
            mailbox.publish(test::makeSolidFrameBuffer(16, 16, shade, sequence));
            while (worker.consumedFrameSequence() != sequence) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            faceLock.probeContentChange(RegionOfInterest{}, frameSize, 0.0);
        }
        worker.stop();
    }
};

// A partial region, so the border has something to outline.
constexpr RegionOfInterest PartialRegion{10.0, 20.0, 60.0, 70.0};

}  // namespace

TEST_CASE("The region kind follows which window the scopes are routed to")
{
    CHECK(regionKind(0) == RegionKind::Global);
    CHECK(regionKind(42) == RegionKind::Attached);
}

TEST_CASE("Full screen is measured on the region's own extent")
{
    CoordinatorFixture fix;
    CHECK(fix.coordinator.isFullScreen());

    fix.region = PartialRegion;
    CHECK_FALSE(fix.coordinator.isFullScreen());

    // A region reaching past the display's edges still reads as full screen.
    fix.region = RegionOfInterest{-5.0, -5.0, 105.0, 105.0};
    CHECK(fix.coordinator.isFullScreen());
}

TEST_CASE("Reading a region the scopes already read asks for nothing")
{
    CoordinatorFixture fix;
    fix.region = PartialRegion;

    // A no-op nudges neither the worker nor the border - it is called every
    // frame the picker previews.
    const RegionOutcome same = fix.coordinator.useRegion(PartialRegion);
    CHECK_FALSE(same.region.has_value());
    CHECK_FALSE(same.activity);

    const RegionOutcome moved = fix.coordinator.useRegion(RegionOfInterest{10.0, 20.0, 60.0, 70.5});
    REQUIRE(moved.region.has_value());
    CHECK_THAT(moved.region->bottomPercent, WithinAbs(70.5, 1e-9));
    CHECK(moved.activity);
}

TEST_CASE("The global region is remembered without being put in force")
{
    CoordinatorFixture fix;
    fix.coordinator.setGlobalRegion(PartialRegion);

    CHECK_THAT(fix.coordinator.globalRegion().leftPercent, WithinAbs(10.0, 1e-9));
    // Storing it is all setGlobalRegion does; the scopes still read what they
    // were reading.
    CHECK(fix.coordinator.isFullScreen());
}

TEST_CASE("Resetting to full screen drops every kind of selection at once")
{
    CoordinatorFixture fix;
    fix.coordinator.setGlobalRegion(PartialRegion);
    (void)fix.attach.attach(42, 100, "Editor", AttachWindowRect{0.0, 0.0, 400.0, 400.0},
                            AttachDisplayRect{0.0, 0.0, 1000.0, 1000.0}, PartialRegion);

    const RegionOutcome outcome = fix.coordinator.resetToFullScreen();

    REQUIRE(outcome.region.has_value());
    CHECK_THAT(outcome.region->rightPercent, WithinAbs(100.0, 1e-9));
    CHECK(outcome.detachedAll);
    CHECK_FALSE(fix.attach.attached());
    // The pending pick goes too, so nothing lands after the reset.
    CHECK(regionOverlayStubs().pickCancels == 1);
    CHECK_THAT(fix.coordinator.globalRegion().rightPercent, WithinAbs(100.0, 1e-9));
}

TEST_CASE("Resetting with nothing attached reports no detach")
{
    CoordinatorFixture fix;
    const RegionOutcome outcome = fix.coordinator.resetToFullScreen();

    CHECK_FALSE(outcome.detachedAll);
    REQUIRE(outcome.region.has_value());
}

TEST_CASE("The border outlines the global region under the display's name")
{
    CoordinatorFixture fix;
    fix.region = PartialRegion;
    desktopStubs().displayName = "Studio Monitor";

    fix.sync();

    REQUIRE(regionOverlayStubs().border.has_value());
    CHECK(regionOverlayStubs().border->displayId == StreamedDisplay);
    CHECK(regionOverlayStubs().border->label == "Studio Monitor");
    CHECK_FALSE(regionOverlayStubs().border->attached);
    CHECK_THAT(regionOverlayStubs().border->region.leftPercent, WithinAbs(10.0, 1e-9));

    // The display's name is read once and kept: it is re-read only when the
    // captured display changes.
    desktopStubs().displayName = "Something Else";
    fix.sync();
    CHECK(regionOverlayStubs().border->label == "Studio Monitor");
}

TEST_CASE("An attached region's border wears the window's own label")
{
    CoordinatorFixture fix;
    fix.region = PartialRegion;

    fix.sync("DSC_0042.NEF", 42);

    REQUIRE(regionOverlayStubs().border.has_value());
    CHECK(regionOverlayStubs().border->attached);
    CHECK(regionOverlayStubs().border->label == "DSC_0042.NEF");
}

TEST_CASE("The border stays off screen while anything says it must")
{
    CoordinatorFixture fix;
    fix.region = PartialRegion;
    fix.sync();
    REQUIRE(regionOverlayStubs().border.has_value());
    const int shownBefore = regionOverlayStubs().borderShows;

    // Each of these on its own takes the border down again. Every one is a
    // way the outline could otherwise be left floating over wrong pixels.
    SECTION("a pick is in flight")
    {
        fix.picker.request(RegionPickerMode::DrawGlobal);
        (void)fix.picker.openIfRequested(true);
        REQUIRE(fix.picker.active());
        fix.sync();
    }
    SECTION("the region covers the whole display")
    {
        fix.region = RegionOfInterest{};
        fix.sync();
    }
    SECTION("this application is hidden")
    {
        desktopStubs().applicationHidden = true;
        fix.sync();
    }
    SECTION("the watched window is moving")
    {
        fix.coordinator.syncBorder(RegionBorderState{"", 0, true, false, 0.0});
    }
    SECTION("this application's own window is minimized")
    {
        fix.coordinator.syncBorder(RegionBorderState{"", 0, false, true, 0.0});
    }
    SECTION("the locked face is not where the region sits")
    {
        fix.faceLock.addLock(42, FaceLockState{}, 0.0);
        fix.faceLock.onActivated(42, 5.0);
        REQUIRE(fix.faceLock.hunting());
        fix.sync();
    }
    SECTION("the locked region's content has not settled")
    {
        fix.unsettleContent();
        REQUIRE(fix.faceLock.contentUnsettled(0.1));
        fix.coordinator.syncBorder(RegionBorderState{"", 0, false, false, 0.1});
    }

    CHECK_FALSE(regionOverlayStubs().border.has_value());
    CHECK(regionOverlayStubs().borderShows == shownBefore);
}

TEST_CASE("No border is drawn while nothing is being captured")
{
    desktopStubs().reset();
    regionOverlayStubs().reset();
    FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    AttachController attach;
    RegionPicker picker{capture, worker, source};
    FaceLockController faceLock{attach, worker, capture};
    const RegionOfInterest region = PartialRegion;
    RegionCoordinator coordinator{attach, capture, picker, faceLock, region};
    REQUIRE(capture.capturedDisplay() == 0);

    // There is no display to draw on, so the border is neither shown nor
    // taken down - the platform side is not touched at all.
    coordinator.syncBorder(RegionBorderState{"", 0, false, false, 0.0});

    CHECK(regionOverlayStubs().borderShows == 0);
    CHECK(regionOverlayStubs().borderHides == 0);
}

TEST_CASE("A border drag stays on the region it began on")
{
    CoordinatorFixture fix;
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    desktopStubs().windowGeometry = WindowGeometry{100.0, 50.0, 400.0, 200.0, false, ""};
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, false, false, PartialRegion};

    // The drag begins while window 42 is focused, so the veil goes up over
    // that window's rectangle: the resize limit made visible.
    const RegionBorderEditOutcome first = fix.coordinator.pollBorderEdit(42);
    CHECK(fix.coordinator.borderEditing());
    CHECK(fix.coordinator.borderEditIdentity() == 42);
    REQUIRE(first.edited.has_value());
    REQUIRE(regionOverlayStubs().editDim.has_value());
    CHECK_THAT(regionOverlayStubs().editDim->leftPercent, WithinAbs(10.0, 1e-9));

    // Focus moves away mid-drag: the edit keeps the identity it latched, so
    // it cannot convert to the global region halfway through, and the veil
    // stays up over the window it belongs to.
    (void)fix.coordinator.pollBorderEdit(0);
    CHECK(fix.coordinator.borderEditIdentity() == 42);
    CHECK(regionOverlayStubs().editDim.has_value());

    // The drag ends: the veil is taken down once, not every frame after.
    regionOverlayStubs().borderEdit = RegionBorderEdit{};
    (void)fix.coordinator.pollBorderEdit(42);
    CHECK_FALSE(fix.coordinator.borderEditing());
    const int hides = regionOverlayStubs().editDimHides;
    (void)fix.coordinator.pollBorderEdit(42);
    CHECK(regionOverlayStubs().editDimHides == hides);
}

TEST_CASE("A global border drag raises no veil")
{
    CoordinatorFixture fix;
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, false, false, PartialRegion};

    // Nothing is focused, so the drag is on the global region: there is no
    // window whose edges limit it and nothing to dim.
    (void)fix.coordinator.pollBorderEdit(0);

    CHECK(fix.coordinator.borderEditing());
    CHECK(fix.coordinator.borderEditIdentity() == 0);
    CHECK_FALSE(regionOverlayStubs().editDim.has_value());
}

TEST_CASE("The border's own affordances travel back to the host")
{
    CoordinatorFixture fix;
    regionOverlayStubs().borderEdit = RegionBorderEdit{false, true, false, std::nullopt};
    CHECK(fix.coordinator.pollBorderEdit(0).dismissed);

    regionOverlayStubs().borderEdit = RegionBorderEdit{false, false, true, std::nullopt};
    CHECK(fix.coordinator.pollBorderEdit(0).attachToggled);
}

TEST_CASE("The border is not read while a pick is in flight")
{
    CoordinatorFixture fix;
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, true, true, PartialRegion};
    fix.picker.request(RegionPickerMode::DrawGlobal);
    (void)fix.picker.openIfRequested(true);
    REQUIRE(fix.picker.active());

    // The border is off screen during a pick, so whatever the platform side
    // still reports about it must not reach the host.
    const RegionBorderEditOutcome outcome = fix.coordinator.pollBorderEdit(42);

    CHECK_FALSE(outcome.dismissed);
    CHECK_FALSE(outcome.attachToggled);
    CHECK_FALSE(outcome.edited.has_value());
    CHECK_FALSE(fix.coordinator.borderEditing());
}

}  // namespace sidescopes
