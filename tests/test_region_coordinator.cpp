#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>
#include <optional>
#include <string>

#include "app/attach_controller.h"
#include "app/capture_controller.h"
#include "app/region_coordinator.h"
#include "app/region_picker.h"
#include "core/analysis_worker.h"
#include "core/frame_mailbox.h"
#include "core/region_kind.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "region_overlay_stubs.h"

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
    std::optional<RegionOfInterest> region;
    RegionCoordinator coordinator{attach, capture, picker, region};

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
    void sync(const std::string& windowLabel = "", uint64_t activeIdentity = 0)
    {
        coordinator.syncBorder(RegionBorderState{windowLabel, activeIdentity, false, false});
    }

    ~CoordinatorFixture()
    {
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

TEST_CASE("Reading a region the scopes already read asks for nothing")
{
    CoordinatorFixture fix;
    fix.region = PartialRegion;

    // A no-op nudges neither the worker nor the border - it is called every
    // frame the picker previews.
    const RegionOutcome same = fix.coordinator.useRegion(PartialRegion);
    CHECK_FALSE(same.regionChanged);
    CHECK_FALSE(same.activity);

    const RegionOutcome moved = fix.coordinator.useRegion(RegionOfInterest{10.0, 20.0, 60.0, 70.5});
    REQUIRE(moved.regionChanged);
    REQUIRE(moved.region.has_value());
    CHECK_THAT(moved.region->bottomPercent, WithinAbs(70.5, 1e-9));
    CHECK(moved.activity);
}

TEST_CASE("Reading no region at all is a change like any other")
{
    CoordinatorFixture fix;
    fix.region = PartialRegion;

    // Empty means two different things on the way out - "read nothing" and
    // "carry on reading" - which is what regionChanged tells apart.
    const RegionOutcome dropped = fix.coordinator.useRegion(std::nullopt);
    CHECK(dropped.regionChanged);
    CHECK_FALSE(dropped.region.has_value());

    fix.region.reset();
    const RegionOutcome already = fix.coordinator.useRegion(std::nullopt);
    CHECK_FALSE(already.regionChanged);
}

TEST_CASE("The global region is remembered without being put in force")
{
    CoordinatorFixture fix;
    fix.coordinator.setGlobalRegion(PartialRegion);

    REQUIRE(fix.coordinator.globalRegion().has_value());
    CHECK_THAT(fix.coordinator.globalRegion()->leftPercent, WithinAbs(10.0, 1e-9));
    // Storing it is all setGlobalRegion does; the scopes still read what they
    // were reading.
    CHECK_FALSE(fix.region.has_value());
}

TEST_CASE("Clearing the region drops every kind of selection at once")
{
    CoordinatorFixture fix;
    fix.region = PartialRegion;
    fix.coordinator.setGlobalRegion(PartialRegion);
    (void)fix.attach.attach(42, 100, "Editor", AttachWindowRect{0.0, 0.0, 400.0, 400.0},
                            AttachDisplayRect{0.0, 0.0, 1000.0, 1000.0}, PartialRegion);

    const RegionOutcome outcome = fix.coordinator.clearRegion();

    CHECK(outcome.regionChanged);
    CHECK_FALSE(outcome.region.has_value());
    CHECK(outcome.detachedAll);
    CHECK_FALSE(fix.attach.attached());
    // The pending pick goes too, so nothing lands after the clear.
    CHECK(regionOverlayStubs().pickCancels == 1);
    CHECK_FALSE(fix.coordinator.globalRegion().has_value());

    fix.region = PartialRegion;
    fix.sync();
    CHECK(regionOverlayStubs().border.has_value());
}

TEST_CASE("Clearing with nothing attached reports no detach")
{
    CoordinatorFixture fix;
    fix.region = PartialRegion;
    const RegionOutcome outcome = fix.coordinator.clearRegion();

    CHECK_FALSE(outcome.detachedAll);
    CHECK(outcome.regionChanged);
}

TEST_CASE("Clearing when the scopes read nothing is not a change")
{
    // An internal reset with no selection must not push settings, resync
    // the border or resave preferences as though a selection had changed.
    CoordinatorFixture fix;
    const RegionOutcome outcome = fix.coordinator.clearRegion();

    CHECK_FALSE(outcome.regionChanged);
    CHECK_FALSE(outcome.detachedAll);
    // An internal reset also cancels pending picker work.
    CHECK(regionOverlayStubs().pickCancels == 1);
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
    CHECK(regionOverlayStubs().border->kind == RegionKind::Global);
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
    CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);
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
        (void)fix.picker.openIfRequested(/*regionSelected=*/false);
        REQUIRE(fix.picker.active());
        fix.sync();
    }
    SECTION("no region is selected")
    {
        fix.region.reset();
        fix.sync();
    }
    SECTION("this application is hidden")
    {
        desktopStubs().applicationHidden = true;
        fix.sync();
    }
    SECTION("the watched window is moving")
    {
        fix.coordinator.syncBorder(RegionBorderState{"", 0, true, false});
    }
    SECTION("this application's own window is minimized")
    {
        fix.coordinator.syncBorder(RegionBorderState{"", 0, false, true});
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
    const std::optional<RegionOfInterest> region = PartialRegion;
    RegionCoordinator coordinator{attach, capture, picker, region};
    REQUIRE(capture.capturedDisplay() == 0);

    // There is no display to draw on, so the border is neither shown nor
    // taken down - the platform side is not touched at all.
    coordinator.syncBorder(RegionBorderState{"", 0, false, false});

    CHECK(regionOverlayStubs().borderShows == 0);
    CHECK(regionOverlayStubs().borderHides == 0);
}

TEST_CASE("A border drag stays on the region it began on")
{
    CoordinatorFixture fix;
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    desktopStubs().windowGeometry = WindowGeometry{100.0, 50.0, 400.0, 200.0, false, ""};
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, false, PartialRegion};

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
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, false, PartialRegion};

    // Nothing is focused, so the drag is on the global region: there is no
    // window whose edges limit it and nothing to dim.
    (void)fix.coordinator.pollBorderEdit(0);

    CHECK(fix.coordinator.borderEditing());
    CHECK(fix.coordinator.borderEditIdentity() == 0);
    CHECK_FALSE(regionOverlayStubs().editDim.has_value());
}

TEST_CASE("The border's binding control travels back to the host")
{
    CoordinatorFixture fix;
    regionOverlayStubs().borderEdit = RegionBorderEdit{false, true, std::nullopt};
    CHECK(fix.coordinator.pollBorderEdit(0).bindingToggled);
}

TEST_CASE("Closing a border takes priority over binding and drag and clears its veil")
{
    CoordinatorFixture fix;
    desktopStubs().displayGeometry = DisplayGeometry{0, 0, 1000, 500};
    desktopStubs().windowGeometry = WindowGeometry{100, 50, 400, 200, false, "Picture"};
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, false, PartialRegion};
    (void)fix.coordinator.pollBorderEdit(42);
    REQUIRE(regionOverlayStubs().editDim);
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, true, PartialRegion, true};
    const auto closed = fix.coordinator.pollBorderEdit(42);
    CHECK(closed.closed);
    CHECK_FALSE(closed.bindingToggled);
    CHECK_FALSE(closed.edited);
    CHECK_FALSE(fix.coordinator.borderEditing());
    CHECK_FALSE(regionOverlayStubs().editDim);
}

TEST_CASE("The border is not read while a pick is in flight")
{
    CoordinatorFixture fix;
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, true, PartialRegion, true};
    fix.picker.request(RegionPickerMode::DrawGlobal);
    (void)fix.picker.openIfRequested(/*regionSelected=*/false);
    REQUIRE(fix.picker.active());

    // The border is off screen during a pick, so whatever the platform side
    // still reports about it must not reach the host.
    const RegionBorderEditOutcome outcome = fix.coordinator.pollBorderEdit(42);

    CHECK_FALSE(outcome.closed);
    CHECK_FALSE(outcome.bindingToggled);
    CHECK_FALSE(outcome.edited.has_value());
    CHECK_FALSE(fix.coordinator.borderEditing());
}

TEST_CASE("A pick or clear ends the attached border's editing veil")
{
    CoordinatorFixture fix;
    desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
    desktopStubs().windowGeometry = WindowGeometry{100.0, 50.0, 400.0, 200.0, false, ""};
    regionOverlayStubs().borderEdit = RegionBorderEdit{true, false, PartialRegion};
    (void)fix.coordinator.pollBorderEdit(42);
    REQUIRE(regionOverlayStubs().editDim.has_value());

    SECTION("picker")
    {
        fix.picker.request(RegionPickerMode::DrawGlobal);
        (void)fix.picker.openIfRequested(false);
        (void)fix.coordinator.pollBorderEdit(42);
    }
    SECTION("clear")
    {
        (void)fix.coordinator.clearRegion();
    }

    CHECK_FALSE(fix.coordinator.borderEditing());
    CHECK_FALSE(regionOverlayStubs().editDim.has_value());
}

}  // namespace sidescopes
