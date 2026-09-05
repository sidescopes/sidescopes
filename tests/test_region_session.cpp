#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/capture_controller.h"
#include "app/region_session.h"
#include "core/analysis_worker.h"
#include "core/frame_mailbox.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "region_overlay_stubs.h"

namespace sidescopes {
namespace {

using test::desktopStubs;
using test::regionOverlayStubs;
constexpr uint32_t Display = 7;
constexpr uint64_t Window = 42;

struct ResetDesktop
{
    ResetDesktop()
    {
        desktopStubs().reset();
        regionOverlayStubs().reset();
    }
};

struct SessionFixture : ResetDesktop
{
    test::FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    RegionSession session{capture, worker, source};

    SessionFixture()
    {
        source.targets = {test::makeTarget(Display, "Display")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(Display);
        REQUIRE(capture.start());
        desktopStubs().displayGeometry = DisplayGeometry{0.0, 0.0, 1000.0, 500.0};
        desktopStubs().cursorDisplay = Display;
        desktopStubs().windowGeometry = WindowGeometry{100.0, 50.0, 400.0, 200.0, false, "Picture"};
        DesktopWindow window;
        window.windowIdentity = Window;
        window.ownerPid = 420;
        window.application = "Editor";
        window.x = 100.0;
        window.y = 50.0;
        window.width = 400.0;
        window.height = 200.0;
        desktopStubs().onScreenWindows = {window};
        desktopStubs().foregroundPid = 420;
        desktopStubs().focusedWindow = Window;
    }

    RegionSessionOutcome pickWindow()
    {
        // Drive the picker without a real capture-frame wait. The confirmed
        // overlay poll still travels through the complete session transition.
        session.picker().request(RegionPickerMode::AttachWindow);
        (void)session.picker().openIfRequested(false);
        regionOverlayStubs().poll.finished = true;
        regionOverlayStubs().poll.displayId = Display;
        regionOverlayStubs().poll.confirmed = RegionOfInterest{10.0, 10.0, 50.0, 50.0};
        const auto outcome = session.poll(false, std::nullopt, std::nullopt);
        regionOverlayStubs().poll = {};
        return outcome;
    }
};

}  // namespace

TEST_CASE("A confirmed attachment owns its focus watch and moves the published region")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    REQUIRE(picked.regionChanged);
    REQUIRE(picked.region);
    CHECK(picked.region->leftPercent > 10.0);
    CHECK(picked.region->rightPercent < 50.0);
    CHECK(desktopStubs().raisedWindow == Window);
    CHECK(fix.session.attachments().isAttached(Window));

    (void)fix.session.follow(false, std::nullopt);
    CHECK(desktopStubs().watchedWindow == Window);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Window);
    CHECK(regionOverlayStubs().border->label == "Picture");

    desktopStubs().foregroundPid = desktopStubs().ownPid;
    desktopStubs().focusedWindow.reset();
    (void)fix.session.follow(false, std::nullopt);
    CHECK(desktopStubs().watchedWindow == Window);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Window);

    desktopStubs().windowGeometry->x += 100.0;
    const auto moved = fix.session.follow(false, std::nullopt);
    REQUIRE(moved.regionChanged);
    REQUIRE(moved.region);
    CHECK(moved.region->leftPercent == Catch::Approx(picked.region->leftPercent + 10.0));
    CHECK(fix.session.carried());
    CHECK_FALSE(regionOverlayStubs().border);
}

TEST_CASE("Removing a moving attachment releases all motion state")
{
    SessionFixture fix;
    (void)fix.pickWindow();
    (void)fix.session.follow(false, std::nullopt);
    REQUIRE(desktopStubs().windowMotion);
    desktopStubs().windowMotion(WindowMotionSignal::Moved);
    REQUIRE(fix.session.carried());

    RegionSessionOutcome removed;
    SECTION("Full reset")
    {
        removed = fix.session.clear();
    }
    SECTION("Border dismissal")
    {
        removed = fix.session.dismiss();
    }
    SECTION("Detach action")
    {
        removed = fix.session.detach();
    }
    CHECK(removed.regionChanged);
    CHECK_FALSE(removed.region);
    CHECK_FALSE(fix.session.attachments().attached());
    CHECK_FALSE(fix.session.carried());
    CHECK(desktopStubs().watchedWindow == 0);
    CHECK_FALSE(desktopStubs().windowMotion);

    const RegionOfInterest next{60.0, 10.0, 90.0, 40.0};
    const auto fresh = fix.session.initializeGlobalRegion(next);
    CHECK(fresh.region == next);
    fix.session.syncBorder(false);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Global);
    CHECK(regionOverlayStubs().border->region == next);
}

TEST_CASE("Binding a global region and releasing it preserves the chosen rectangle")
{
    SessionFixture fix;
    const RegionOfInterest selected{20.0, 20.0, 40.0, 40.0};
    (void)fix.session.initializeGlobalRegion(selected);
    regionOverlayStubs().borderEdit.bindingToggled = true;
    (void)fix.session.poll(false, std::nullopt, std::nullopt);
    regionOverlayStubs().borderEdit = {};
    CHECK(fix.session.attachments().isAttached(Window));
    (void)fix.session.follow(false, std::nullopt);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Window);
    CHECK(regionOverlayStubs().border->region == selected);

    regionOverlayStubs().borderEdit.bindingToggled = true;
    (void)fix.session.poll(false, std::nullopt, std::nullopt);
    CHECK_FALSE(fix.session.attachments().attached());
    CHECK(desktopStubs().watchedWindow == 0);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Global);
    CHECK(regionOverlayStubs().border->region == selected);
}

TEST_CASE("A region session releases native callbacks when destroyed")
{
    {
        SessionFixture fix;
        (void)fix.pickWindow();
        (void)fix.session.follow(false, std::nullopt);
        REQUIRE(desktopStubs().windowMotion);
    }
    CHECK(desktopStubs().watchedWindow == 0);
    CHECK_FALSE(desktopStubs().windowMotion);
}

}  // namespace sidescopes
