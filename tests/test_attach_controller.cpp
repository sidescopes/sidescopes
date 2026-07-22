#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/attach_controller.h"

namespace sidescopes {

using Catch::Approx;

namespace {

// A 1000x1000 display at the origin: desktop points map to percentages one for
// ten, so the expected regions read directly off the geometry.
constexpr AttachDisplayRect PrimaryDisplay{0.0, 0.0, 1000.0, 1000.0};
constexpr uint32_t PrimaryDisplayId = 1;
// A second display to its right, same size, for the display-move case.
constexpr AttachDisplayRect SecondaryDisplay{1000.0, 0.0, 1000.0, 1000.0};
constexpr uint32_t SecondaryDisplayId = 2;

// The attached editor windows: desktop 100..500 and 500..900 on both axes.
constexpr AttachWindowRect EditorWindow{100.0, 100.0, 400.0, 400.0};
constexpr AttachWindowRect SecondWindow{500.0, 500.0, 400.0, 400.0};

constexpr int64_t EditorPid = 200;

// A plain window pick confirms the window's own rectangle as the region.
constexpr RegionOfInterest WholeEditor{10.0, 10.0, 50.0, 50.0};
constexpr RegionOfInterest WholeSecond{50.0, 50.0, 90.0, 90.0};

void checkRegion(const RegionOfInterest& region, double left, double top, double right, double bottom)
{
    CHECK(region.leftPercent == Approx(left));
    CHECK(region.topPercent == Approx(top));
    CHECK(region.rightPercent == Approx(right));
    CHECK(region.bottomPercent == Approx(bottom));
}

AttachedWindowObservation visibleWindow(uint64_t identity, AttachWindowRect rect, uint32_t displayId = PrimaryDisplayId,
                                        AttachDisplayRect display = PrimaryDisplay)
{
    AttachedWindowObservation observation;
    observation.identity = identity;
    observation.windowRect = rect;
    observation.displayId = displayId;
    observation.display = display;

    return observation;
}

AttachedWindowObservation minimizedWindow(uint64_t identity, AttachWindowRect rect)
{
    AttachedWindowObservation observation;
    observation.identity = identity;
    observation.windowRect = rect;
    observation.minimized = true;

    return observation;
}

AttachedWindowObservation closedWindow(uint64_t identity)
{
    AttachedWindowObservation observation;
    observation.identity = identity;

    return observation;
}

}  // namespace

TEST_CASE("Attaching a window maps its region to display percent")
{
    AttachController controller;
    REQUIRE_FALSE(controller.attached());

    const RegionOfInterest mapped =
        controller.attach(42, EditorPid, "Lightroom", EditorWindow, PrimaryDisplay, WholeEditor);

    CHECK(controller.attached());
    CHECK(controller.attachedCount() == 1);
    CHECK(controller.activeIdentity() == 42);
    CHECK(controller.activeApplicationName() == "Lightroom");
    checkRegion(mapped, 10.0, 10.0, 50.0, 50.0);
}

TEST_CASE("A moved window carries the region")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    const AttachWindowRect moved{200.0, 100.0, 400.0, 400.0};
    const AttachDecision decision = controller.observe({visibleWindow(42, moved)}, 42);

    CHECK(decision.activeIdentity == 42);
    CHECK(decision.activeOwnerPid == EditorPid);
    REQUIRE(decision.region.has_value());
    checkRegion(*decision.region, 20.0, 10.0, 60.0, 50.0);
}

TEST_CASE("A resized window leaves the region glued to the screen")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    // The window doubles in width: the region does not budge - a resize is
    // not a move, and no edge reached it.
    const AttachWindowRect wider{100.0, 100.0, 800.0, 400.0};
    const AttachDecision decision = controller.observe({visibleWindow(42, wider)}, 42);

    REQUIRE(decision.region.has_value());
    checkRegion(*decision.region, 10.0, 10.0, 50.0, 50.0);
}

TEST_CASE("A face-sized region ignores a resize that never reaches it")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    // The face: desktop 200..300 on both axes.
    const RegionOfInterest face{20.0, 20.0, 30.0, 30.0};
    controller.editRegion(face, EditorWindow, PrimaryDisplay);

    // The window grows to the right by 400: the face on screen did not
    // move, and neither does the region.
    const AttachWindowRect wider{100.0, 100.0, 800.0, 400.0};
    const AttachDecision grown = controller.observe({visibleWindow(42, wider)}, 42);
    REQUIRE(grown.region.has_value());
    checkRegion(*grown.region, 20.0, 20.0, 30.0, 30.0);

    // Moving the grown window still carries the region exactly.
    const AttachWindowRect movedWider{200.0, 100.0, 800.0, 400.0};
    const AttachDecision moved = controller.observe({visibleWindow(42, movedWider)}, 42);
    REQUIRE(moved.region.has_value());
    checkRegion(*moved.region, 30.0, 20.0, 40.0, 30.0);
}

TEST_CASE("An arriving window edge pushes the region, permanently")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    const RegionOfInterest face{20.0, 20.0, 30.0, 30.0};
    controller.editRegion(face, EditorWindow, PrimaryDisplay);

    // The left edge drags right past the region's place: the region gets
    // pushed along, keeping its size.
    const AttachWindowRect narrowed{250.0, 100.0, 250.0, 400.0};
    const AttachDecision pushed = controller.observe({visibleWindow(42, narrowed)}, 42);
    REQUIRE(pushed.region.has_value());
    checkRegion(*pushed.region, 25.0, 20.0, 35.0, 30.0);

    // The edge retreats: pushed means pushed - the region stays where the
    // edge left it, like an object on glass.
    const AttachDecision retreated = controller.observe({visibleWindow(42, EditorWindow)}, 42);
    REQUIRE(retreated.region.has_value());
    checkRegion(*retreated.region, 25.0, 20.0, 35.0, 30.0);
}

TEST_CASE("Walls smaller than the region squeeze it elastically")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    const RegionOfInterest face{20.0, 20.0, 30.0, 30.0};
    controller.editRegion(face, EditorWindow, PrimaryDisplay);

    // The window shrinks below the region's size: the region is pushed to
    // the surviving corner and the emitted mapping clips.
    const AttachWindowRect tiny{100.0, 100.0, 80.0, 80.0};
    const AttachDecision squeezed = controller.observe({visibleWindow(42, tiny)}, 42);
    REQUIRE(squeezed.region.has_value());
    checkRegion(*squeezed.region, 10.0, 10.0, 18.0, 18.0);

    // The walls part: the size re-expands (the clip was elastic), from the
    // pushed position (the push was not).
    const AttachDecision expanded = controller.observe({visibleWindow(42, EditorWindow)}, 42);
    REQUIRE(expanded.region.has_value());
    checkRegion(*expanded.region, 10.0, 10.0, 20.0, 20.0);
}

TEST_CASE("A focused attached window always carries its mapping")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    // The mapping is re-emitted every frame - deduplication is the host's
    // job - so a missed edge can never strand a stale region.
    const AttachDecision decision = controller.observe({visibleWindow(42, EditorWindow)}, 42);

    CHECK(decision.activeIdentity == 42);
    REQUIRE(decision.region.has_value());
    checkRegion(*decision.region, 10.0, 10.0, 50.0, 50.0);
}

TEST_CASE("Focus on an unattached window yields no attached region")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    // The focused window is an unattached sibling (a second Preview
    // document) or SideScopes itself: the attached region must neither show
    // nor be effective - the host falls back to the global region.
    const AttachDecision sibling = controller.observe({visibleWindow(42, EditorWindow)}, 7);
    CHECK(sibling.activeIdentity == 0);
    CHECK_FALSE(sibling.region.has_value());

    const AttachDecision unknown = controller.observe({visibleWindow(42, EditorWindow)}, std::nullopt);
    CHECK(unknown.activeIdentity == 0);
    CHECK_FALSE(unknown.region.has_value());
}

TEST_CASE("A focused window that is minimized cannot be active")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    // A race between the focus read and a minimize: the observation wins.
    const AttachDecision decision = controller.observe({minimizedWindow(42, EditorWindow)}, 42);

    CHECK(decision.activeIdentity == 0);
    CHECK_FALSE(decision.region.has_value());
}

TEST_CASE("An in-window region edit re-anchors the stored rect without detaching")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    const RegionOfInterest drawn{20.0, 20.0, 40.0, 40.0};
    const RegionOfInterest stored = controller.editRegion(drawn, EditorWindow, PrimaryDisplay);

    CHECK(controller.attached());
    checkRegion(stored, 20.0, 20.0, 40.0, 40.0);

    // The window moves; the sub-rectangle moves with it (relative fraction
    // preserved), proving the edit updated the relative rect.
    const AttachWindowRect moved{300.0, 100.0, 400.0, 400.0};
    const AttachDecision decision = controller.observe({visibleWindow(42, moved)}, 42);

    REQUIRE(decision.region.has_value());
    checkRegion(*decision.region, 40.0, 20.0, 60.0, 40.0);
}

TEST_CASE("Drawing past the window clamps the region to the window")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    const RegionOfInterest wholeDisplay{0.0, 0.0, 100.0, 100.0};
    const RegionOfInterest stored = controller.editRegion(wholeDisplay, EditorWindow, PrimaryDisplay);

    checkRegion(stored, 10.0, 10.0, 50.0, 50.0);
}

TEST_CASE("A hidden window keeps its region and stops being active")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    // Hidden or minimized: no attached region is emitted, the window stays
    // attached.
    const AttachDecision decision = controller.observe({minimizedWindow(42, EditorWindow)}, std::nullopt);

    CHECK(decision.activeIdentity == 0);
    CHECK_FALSE(decision.region.has_value());
    CHECK(controller.attached());

    // The window returns to focus: its region comes back exactly where it
    // was.
    const AttachDecision back = controller.observe({visibleWindow(42, EditorWindow)}, 42);
    CHECK(back.activeIdentity == 42);
    REQUIRE(back.region.has_value());
    checkRegion(*back.region, 10.0, 10.0, 50.0, 50.0);
}

TEST_CASE("Two attached windows keep independent regions and follow the front one")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    controller.attach(7, EditorPid, "Editor", SecondWindow, PrimaryDisplay, WholeSecond);
    CHECK(controller.attachedCount() == 2);

    const std::vector<AttachedWindowObservation> both{visibleWindow(42, EditorWindow), visibleWindow(7, SecondWindow)};

    // The second window is in front right after its pick; switching the
    // front window switches the analyzed region, both regions intact.
    const AttachDecision toFirst = controller.observe(both, 42);
    CHECK(toFirst.activeIdentity == 42);
    REQUIRE(toFirst.region.has_value());
    checkRegion(*toFirst.region, 10.0, 10.0, 50.0, 50.0);

    const AttachDecision toSecond = controller.observe(both, 7);
    CHECK(toSecond.activeIdentity == 7);
    REQUIRE(toSecond.region.has_value());
    checkRegion(*toSecond.region, 50.0, 50.0, 90.0, 90.0);
}

TEST_CASE("An edit binds to the active window only")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    controller.attach(7, EditorPid, "Editor", SecondWindow, PrimaryDisplay, WholeSecond);

    const std::vector<AttachedWindowObservation> both{visibleWindow(42, EditorWindow), visibleWindow(7, SecondWindow)};
    controller.observe(both, 42);

    // Editing while the first window is active narrows only its region.
    const RegionOfInterest drawn{20.0, 20.0, 40.0, 40.0};
    controller.editRegion(drawn, EditorWindow, PrimaryDisplay);

    const AttachDecision toSecond = controller.observe(both, 7);
    REQUIRE(toSecond.region.has_value());
    checkRegion(*toSecond.region, 50.0, 50.0, 90.0, 90.0);

    const AttachDecision toFirst = controller.observe(both, 42);
    REQUIRE(toFirst.region.has_value());
    checkRegion(*toFirst.region, 20.0, 20.0, 40.0, 40.0);
}

TEST_CASE("Re-picking an attached window updates its region in place")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    const RegionOfInterest narrower{20.0, 20.0, 40.0, 40.0};
    const RegionOfInterest stored = controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, narrower);

    CHECK(controller.attachedCount() == 1);
    checkRegion(stored, 20.0, 20.0, 40.0, 40.0);
}

TEST_CASE("With no attached window visible no region is emitted")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    controller.attach(7, EditorPid, "Editor", SecondWindow, PrimaryDisplay, WholeSecond);

    const AttachDecision decision =
        controller.observe({minimizedWindow(42, EditorWindow), minimizedWindow(7, SecondWindow)}, std::nullopt);

    CHECK(decision.activeIdentity == 0);
    CHECK_FALSE(decision.region.has_value());
    CHECK(controller.attached());
}

TEST_CASE("One of two windows closing prunes it and keeps the other attached")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    controller.attach(7, EditorPid, "Editor", SecondWindow, PrimaryDisplay, WholeSecond);

    const AttachDecision decision = controller.observe({closedWindow(42), visibleWindow(7, SecondWindow)}, 7);

    CHECK(decision.closedCount == 1);
    CHECK_FALSE(decision.detachedAll);
    CHECK(controller.attachedCount() == 1);
    CHECK(decision.activeIdentity == 7);
}

TEST_CASE("The last window closing detaches; the host falls back to the global region")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    const AttachWindowRect moved{200.0, 100.0, 400.0, 400.0};
    controller.observe({visibleWindow(42, moved)}, 42);

    const AttachDecision decision = controller.observe({closedWindow(42)}, std::nullopt);

    CHECK(decision.closedCount == 1);
    CHECK(decision.detachedAll);
    CHECK_FALSE(controller.attached());
    CHECK_FALSE(decision.region.has_value());
}

TEST_CASE("Removing one window keeps the rest attached")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    controller.attach(7, EditorPid, "Editor", SecondWindow, PrimaryDisplay, WholeSecond);

    controller.remove(7);

    CHECK(controller.attachedCount() == 1);
    CHECK(controller.activeIdentity() == 0);

    const AttachDecision decision = controller.observe({visibleWindow(42, EditorWindow)}, 42);
    CHECK(decision.activeIdentity == 42);
}

TEST_CASE("Detaching everything clears the set")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);
    controller.attach(7, EditorPid, "Editor", SecondWindow, PrimaryDisplay, WholeSecond);

    controller.detachAll();

    CHECK_FALSE(controller.attached());
    CHECK(controller.attachedCount() == 0);
    CHECK(controller.activeIdentity() == 0);
}

TEST_CASE("The window moving to another display re-maps the region there")
{
    AttachController controller;
    controller.attach(42, EditorPid, "Editor", EditorWindow, PrimaryDisplay, WholeEditor);

    const AttachWindowRect onSecondary{1200.0, 200.0, 400.0, 400.0};
    const AttachDecision decision =
        controller.observe({visibleWindow(42, onSecondary, SecondaryDisplayId, SecondaryDisplay)}, 42);

    CHECK(decision.activeDisplayId == SecondaryDisplayId);
    REQUIRE(decision.region.has_value());
    checkRegion(*decision.region, 20.0, 20.0, 60.0, 60.0);
}

TEST_CASE("A detached controller ignores observations and edits")
{
    AttachController controller;

    const AttachDecision decision = controller.observe({visibleWindow(42, EditorWindow)}, 42);
    CHECK(decision.activeIdentity == 0);
    CHECK_FALSE(decision.detachedAll);

    const RegionOfInterest passthrough = controller.editRegion(WholeEditor, EditorWindow, PrimaryDisplay);
    checkRegion(passthrough, 10.0, 10.0, 50.0, 50.0);

    controller.detachAll();
    CHECK_FALSE(controller.attached());
}

TEST_CASE("A reopening animation never pushes the region")
{
    AttachController controller;
    (void)controller.attach(1, 100, "Editor", {200.0, 100.0, 800.0, 600.0}, PrimaryDisplay,
                            RegionOfInterest{25.0, 15.0, 45.0, 35.0});

    // Hidden, then animated back over several transitional rectangles -
    // Quick Look zooms its panel open - before landing where it was.
    (void)controller.observe({minimizedWindow(1, {200.0, 100.0, 800.0, 600.0})}, 1);
    (void)controller.observe({visibleWindow(1, {450.0, 300.0, 200.0, 150.0})}, 1);
    (void)controller.observe({visibleWindow(1, {300.0, 180.0, 560.0, 420.0})}, 1);
    (void)controller.observe({visibleWindow(1, {200.0, 100.0, 800.0, 600.0})}, 1);
    const AttachDecision landed = controller.observe({visibleWindow(1, {200.0, 100.0, 800.0, 600.0})}, 1);

    REQUIRE(landed.region.has_value());
    checkRegion(*landed.region, 25.0, 15.0, 45.0, 35.0);
}

TEST_CASE("A window moved while hidden leaves the region screen-glued")
{
    AttachController controller;
    (void)controller.attach(1, 100, "Editor", {200.0, 100.0, 800.0, 600.0}, PrimaryDisplay,
                            RegionOfInterest{25.0, 15.0, 45.0, 35.0});

    (void)controller.observe({minimizedWindow(1, {200.0, 100.0, 800.0, 600.0})}, 1);
    (void)controller.observe({visibleWindow(1, {400.0, 100.0, 800.0, 600.0})}, 1);
    const AttachDecision landed = controller.observe({visibleWindow(1, {400.0, 100.0, 800.0, 600.0})}, 1);

    // The re-appearance is a baseline, not a move: the region held its
    // screen position and only the emitted mapping clips to the window.
    REQUIRE(landed.region.has_value());
    checkRegion(*landed.region, 40.0, 15.0, 45.0, 35.0);
}

}  // namespace sidescopes
