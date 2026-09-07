#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "app/capture_controller.h"
#include "app/region_session.h"
#include "core/analysis_worker.h"
#include "core/frame_mailbox.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "region_overlay_stubs.h"
#include "test_frame.h"

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

struct FaceSessionFixture : SessionFixture
{
    AnalysisSettings settings;
    const AnalysisWorker::FrameSize frameSize{1000, 500, 1000, 500};

    FaceSessionFixture()
    {
        desktopStubs().faceDetectionSupported = true;
        desktopStubs().faces = {{200, 100, 100, 100}};
        settings.region = RegionOfInterest{};
        settings.enabledScopes = {"org.sidescopes.histogram"};
        worker.updateSettings(settings);
        worker.startInline();
        publish(1);
    }

    void publish(uint64_t sequence)
    {
        auto frame = test::makeSolidFrameBuffer(1000, 500, Color{40, 80, 120}, sequence);
        frame.stamp = {capture.streamEpoch(), capture.capturedDisplay(), frameClockSeconds()};
        mailbox.publish(std::move(frame));
        worker.pump();
    }

    void open()
    {
        session.picker().request(RegionPickerMode::AttachFace);
        (void)session.picker().openIfRequested(false);
    }

    RegionSessionOutcome confirm(uint32_t display, RegionOfInterest region)
    {
        regionOverlayStubs().poll = {};
        regionOverlayStubs().poll.displayId = display;
        regionOverlayStubs().poll.confirmed = region;
        regionOverlayStubs().poll.finished = true;
        const auto result = session.poll(false, frameSize, {});
        regionOverlayStubs().poll = {};
        return result;
    }

    RegionSessionOutcome follow()
    {
        const auto result = session.follow(false, frameSize);
        settings.region = result.region;
        settings.selectionRevision = result.selectionRevision;
        worker.updateSettings(settings);
        return result;
    }
};

}  // namespace

TEST_CASE("A stale face confirmation restores the committed crop before replacing capture")
{
    FaceSessionFixture fix;
    const RegionOfInterest committed{60, 60, 90, 90};
    (void)fix.session.initializeGlobalRegion(committed);
    fix.open();
    REQUIRE(regionOverlayStubs().lastDisplays[0].faces.size() == 1);
    const auto suggestion = regionOverlayStubs().lastDisplays[0].faces[0].region;
    regionOverlayStubs().poll.active = true;
    regionOverlayStubs().poll.displayId = Display;
    regionOverlayStubs().poll.preview = suggestion;
    REQUIRE(fix.session.poll(false, fix.frameSize, {}).region == suggestion);
    SECTION("Parent moved")
    {
        desktopStubs().windowGeometry->x += 10;
    }
    SECTION("Display geometry changed")
    {
        desktopStubs().displayGeometry->heightPoints += 10;
    }
    SECTION("Parent became unavailable")
    {
        desktopStubs().windowGeometry.reset();
    }
    SECTION("Source restarted")
    {
        REQUIRE(fix.capture.start());
    }
    const auto epochBeforeConfirm = fix.capture.streamEpoch();
    const auto result = fix.confirm(Display, suggestion);
    CHECK(result.region == committed);
    REQUIRE(result.status);
    CHECK(*result.status == "selection source changed - previous region kept");
    CHECK_FALSE(fix.session.faceLocked());
    CHECK_FALSE(fix.session.attachments().attached());
    CHECK(fix.capture.streamEpoch() == epochBeforeConfirm);
    CHECK(fix.capture.desiredDisplay() == Display);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == committed);
}

TEST_CASE("Snapshot face selection is mapped to live pixels and automatic motion keeps its revision")
{
    FaceSessionFixture fix;
    constexpr uint32_t SecondDisplay = Display + 1;
    fix.source.targets.push_back(test::makeTarget(SecondDisplay, "Second display"));
    constexpr int SnapshotWidth = 500;
    int snapshotHeight = 250;
    SECTION("Uniform scale")
    {
    }
    SECTION("Different horizontal and vertical scale")
    {
        snapshotHeight = 125;
    }
    desktopStubs().faces = {{100, snapshotHeight / 5, 50, snapshotHeight / 5}};
    desktopStubs().displayImage = CapturedImage{
        PixelStorage(static_cast<std::size_t>(SnapshotWidth) * snapshotHeight * 4, 40), SnapshotWidth, snapshotHeight};
    fix.open();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fix.session.picker().scansRunning() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE_FALSE(fix.session.picker().scansRunning());
    fix.session.picker().drainFaceScans();
    const auto& suggestions = regionOverlayStubs().deliveredFaces.at(SecondDisplay);
    REQUIRE(suggestions.size() == 1);
    // This stub answers displayAtPoint for window centres too: the selected
    // parent is on the second display, not the fixture's original display.
    desktopStubs().cursorDisplay = SecondDisplay;
    const auto picked = fix.confirm(SecondDisplay, suggestions[0].region);
    REQUIRE(fix.session.faceLocked());
    REQUIRE(picked.region);
    REQUIRE(fix.capture.capturedDisplay() == SecondDisplay);
    const auto selectedEpoch = fix.capture.streamEpoch();
    // The snapshot anchor (125, 0.3 * height), width 50, maps to (250,150), width100.
    desktopStubs().sessionDetection = [](const FrameView& crop, double) {
        CHECK(crop.stamp.displayId == SecondDisplay);
        return FaceDetectionResult{FaceDetectionStatus::Completed,
                                   {{200 - crop.sourceX, 100 - crop.sourceY, 100, 100}}};
    };
    (void)fix.follow();
    REQUIRE(desktopStubs().lastDisplayPoint);
    CHECK(desktopStubs().lastDisplayPoint->x == 300.0);
    CHECK(desktopStubs().lastDisplayPoint->y == 150.0);
    CHECK(fix.capture.streamEpoch() == selectedEpoch);
    fix.publish(2);
    const auto first = fix.follow();
    REQUIRE(fix.session.faceLocked());
    CHECK(first.region == picked.region);
    const auto revision = first.selectionRevision;
    desktopStubs().sessionDetection = [](const FrameView& crop, double) {
        CHECK(crop.stamp.displayId == SecondDisplay);
        return FaceDetectionResult{FaceDetectionStatus::Completed,
                                   {{205 - crop.sourceX, 100 - crop.sourceY, 100, 100}}};
    };
    fix.publish(3);
    const auto moved = fix.follow();
    REQUIRE(moved.regionChanged);
    CHECK(moved.trackedRegion);
    CHECK(moved.selectionRevision == revision);
    REQUIRE(moved.region);
    CHECK(moved.region->leftPercent == Catch::Approx(picked.region->leftPercent + 0.5));
    AnalysisWorker::Output output;
    uint64_t seen = 0;
    REQUIRE(fix.worker.fetchOutput(seen, output, revision));
    CHECK(output.frameSequence == 3);
    CHECK(output.region == moved.region);
    CHECK(output.frameStamp.displayId == SecondDisplay);
    CHECK(output.frameStamp.captureEpoch == selectedEpoch);
}

TEST_CASE("An impossible face nomination retires to the ordinary mapped attachment")
{
    FaceSessionFixture fix;
    desktopStubs().windowGeometry = WindowGeometry{240, 50, 20, 200, false, "Narrow window"};
    auto& host = desktopStubs().onScreenWindows.front();
    host.x = 240;
    host.width = 20;
    fix.open();
    REQUIRE(regionOverlayStubs().lastDisplays[0].faces.size() == 1);
    const auto picked = fix.confirm(Display, regionOverlayStubs().lastDisplays[0].faces[0].region);
    REQUIRE(picked.region);
    (void)fix.follow();
    fix.publish(2);
    const auto ordinary = fix.follow();
    CHECK_FALSE(fix.session.faceLocked());
    CHECK(fix.session.attachments().isAttached(Window));
    CHECK(ordinary.region == picked.region);
    CHECK(ordinary.region->leftPercent >= 24.0);
    CHECK(ordinary.region->rightPercent <= 26.0);
}

TEST_CASE("A face selection survives animated minimization and an empty cancelled picker")
{
    FaceSessionFixture fix;
    double now = 1.0;
    desktopStubs().clock = [&] { return now; };
    desktopStubs().sessionDetection = [](const FrameView& crop, double) {
        return FaceDetectionResult{FaceDetectionStatus::Completed,
                                   {{200 - crop.sourceX, 100 - crop.sourceY, 100, 100}}};
    };
    fix.open();
    REQUIRE(regionOverlayStubs().lastDisplays[0].faces.size() == 1);
    const auto picked = fix.confirm(Display, regionOverlayStubs().lastDisplays[0].faces[0].region);
    REQUIRE(picked.region);
    (void)fix.follow();
    fix.publish(2);
    const auto accepted = fix.follow();
    REQUIRE(accepted.region);
    const auto original = *desktopStubs().windowGeometry;

    now = 2.0;
    desktopStubs().windowGeometry = WindowGeometry{700, 400, 100, 50, false, "Picture"};
    (void)fix.follow();
    CHECK(fix.session.faceLocked());
    now = 2.05;
    desktopStubs().windowGeometry->minimized = true;
    CHECK_FALSE(fix.follow().region);
    CHECK_FALSE(regionOverlayStubs().border);
    desktopStubs().onScreenWindows.clear();
    desktopStubs().faces.clear();
    fix.open();
    REQUIRE(regionOverlayStubs().lastDisplays[0].faces.empty());
    (void)fix.session.cancel();
    CHECK(fix.session.faceLocked());
    CHECK(fix.session.attachments().isAttached(Window));
    CHECK_FALSE(regionOverlayStubs().border);

    now = 5.0;
    desktopStubs().windowGeometry->minimized = false;
    CHECK_FALSE(fix.follow().region);
    CHECK_FALSE(fix.follow().region);  // same UI frame is not a settled window
    CHECK(fix.session.faceLocked());
    now = 5.05;
    desktopStubs().windowGeometry = original;
    CHECK_FALSE(fix.follow().region);
    now = 5.3;
    const auto restored = fix.follow();
    REQUIRE(restored.region);
    CHECK(restored.region->toPixels(1000, 500) == accepted.region->toPixels(1000, 500));
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Face);
    fix.publish(3);
    const auto resumed = fix.follow();
    REQUIRE(resumed.region);
    CHECK(resumed.region->toPixels(1000, 500) == accepted.region->toPixels(1000, 500));
    CHECK(fix.session.faceLocked());
    desktopStubs().clock = {};
}

TEST_CASE("Cancelling an empty face picker resumes tracking across paused capture streams")
{
    FaceSessionFixture fix;
    double now = 1.0;
    bool advancePickerWait = false;
    desktopStubs().clock = [&] {
        // Advance only the picker's bounded native-frame wait. No wall-clock
        // sleeps or real capture threads are needed for this lifecycle test.
        if (advancePickerWait) {
            now += 0.31;
        }
        return now;
    };
    desktopStubs().sessionDetection = [](const FrameView& crop, double) {
        return FaceDetectionResult{FaceDetectionStatus::Completed,
                                   {{200 - crop.sourceX, 100 - crop.sourceY, 100, 100}}};
    };
    fix.open();
    const auto picked = fix.confirm(Display, regionOverlayStubs().lastDisplays[0].faces[0].region);
    REQUIRE(picked.region);
    (void)fix.follow();
    fix.publish(2);
    const auto accepted = fix.follow();
    REQUIRE(accepted.region);
    const auto firstEpoch = fix.capture.streamEpoch();
    const auto continuity = fix.capture.continuityGeneration();
    const int calls = desktopStubs().detectorCall().calls;

    desktopStubs().windowGeometry->minimized = true;
    desktopStubs().onScreenWindows.clear();
    desktopStubs().faces.clear();
    CHECK_FALSE(fix.follow().region);
    fix.capture.suspend("waiting for a region source");
    fix.worker.releaseFrame();
    fix.session.picker().request(RegionPickerMode::AttachFace);
    regionOverlayStubs().poll.active = true;
    advancePickerWait = true;
    (void)fix.session.poll(false, fix.frameSize, {});
    advancePickerWait = false;
    REQUIRE(fix.session.picker().active());
    CHECK(regionOverlayStubs().lastDisplays[0].faces.empty());
    CHECK(fix.capture.streamEpoch() > firstEpoch);
    CHECK(fix.capture.continuityGeneration() == continuity);
    const auto pickerEpoch = fix.capture.streamEpoch();
    regionOverlayStubs().poll = {};
    (void)fix.session.cancel();
    CHECK(fix.session.faceLocked());
    fix.capture.suspend("waiting for a region source");
    fix.worker.releaseFrame();

    desktopStubs().windowGeometry->minimized = false;
    now += 1.0;
    CHECK_FALSE(fix.follow().region);
    now += 0.3;
    REQUIRE(fix.follow().region);
    fix.capture.resume();
    REQUIRE(fix.capture.streamEpoch() > pickerEpoch);
    REQUIRE(fix.capture.continuityGeneration() == continuity);
    (void)fix.follow();
    auto late = test::makeSolidFrameBuffer(1000, 500, Color{40, 80, 120}, 3);
    late.stamp = {firstEpoch, Display, frameClockSeconds()};
    fix.mailbox.publish(std::move(late));
    fix.worker.pump();
    CHECK(desktopStubs().detectorCall().calls == calls);
    CHECK(fix.session.faceLocked());
    fix.publish(1);  // fresh stream numbering may restart
    const auto resumed = fix.follow();
    REQUIRE(resumed.region);
    CHECK(resumed.region->toPixels(1000, 500) == accepted.region->toPixels(1000, 500));
    CHECK(fix.session.faceLocked());
    CHECK(desktopStubs().detectorCall().calls == calls + 1);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Face);
    desktopStubs().clock = {};
}

TEST_CASE("Closure during a minimize animation keeps the last settled attached crop")
{
    SessionFixture fix;
    double now = 1.0;
    desktopStubs().clock = [&] { return now; };
    const auto picked = fix.pickWindow();
    REQUIRE(picked.region);
    (void)fix.session.follow(false, {});
    now = 2.0;
    desktopStubs().windowGeometry = WindowGeometry{700, 400, 100, 50, false, "Picture"};
    (void)fix.session.follow(false, {});
    desktopStubs().windowGeometry.reset();
    desktopStubs().windowPresence = WindowPresence::Closed;
    const auto closed = fix.session.follow(false, {});
    CHECK(closed.region == picked.region);
    CHECK_FALSE(fix.session.attachments().attached());
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Global);
    desktopStubs().clock = {};
}

TEST_CASE("Stale border actions cannot replace a face crop during parent motion")
{
    FaceSessionFixture fix;
    double now = 1.0;
    desktopStubs().clock = [&] { return now; };
    fix.open();
    const auto picked = fix.confirm(Display, regionOverlayStubs().lastDisplays[0].faces[0].region);
    REQUIRE(picked.region);
    (void)fix.follow();
    // Latch the attached edit identity while the border is still usable.
    regionOverlayStubs().borderEdit.editing = true;
    (void)fix.session.poll(false, fix.frameSize, {});
    regionOverlayStubs().borderEdit = {};
    const auto original = *desktopStubs().windowGeometry;
    now = 2.0;
    SECTION("The follow step already hid the moving border")
    {
        desktopStubs().windowGeometry = WindowGeometry{700, 400, 100, 50, false, "Picture"};
        (void)fix.follow();
    }
    SECTION("The parent changes between following and polling the border")
    {
        desktopStubs().windowGeometry = WindowGeometry{700, 400, 100, 50, false, "Picture"};
    }
    SECTION("The parent becomes minimized before its queued edit is polled")
    {
        desktopStubs().windowGeometry->minimized = true;
    }
    SECTION("The parent geometry is unavailable before its queued edit is polled")
    {
        desktopStubs().windowGeometry.reset();
    }
    regionOverlayStubs().borderEdit.region = RegionOfInterest{70, 80, 75, 85};
    (void)fix.session.poll(false, fix.frameSize, {});
    regionOverlayStubs().borderEdit = {};
    regionOverlayStubs().borderEdit.bindingToggled = true;
    (void)fix.session.poll(false, fix.frameSize, {});
    regionOverlayStubs().borderEdit = {};
    CHECK(fix.session.faceLocked());
    CHECK(fix.session.attachments().isAttached(Window));
    // Hiding aborts the animation; neither queued edit may have replaced
    // the rollback origin, last accepted crop or face binding.
    desktopStubs().windowGeometry = original;
    desktopStubs().windowGeometry->minimized = true;
    now = 2.05;
    (void)fix.follow();
    desktopStubs().windowGeometry = original;
    now = 3.0;
    (void)fix.follow();
    now = 3.3;
    const auto restored = fix.follow();
    REQUIRE(restored.region);
    CHECK(restored.region->toPixels(1000, 500) == picked.region->toPixels(1000, 500));
    CHECK(fix.session.faceLocked());
    desktopStubs().clock = {};
}

TEST_CASE("Stable border edits and face release still apply to the selected parent")
{
    FaceSessionFixture fix;
    fix.open();
    const auto picked = fix.confirm(Display, regionOverlayStubs().lastDisplays[0].faces[0].region);
    REQUIRE(picked.region);
    (void)fix.follow();
    const RegionOfInterest edited{22, 22, 28, 34};
    regionOverlayStubs().borderEdit.editing = true;
    regionOverlayStubs().borderEdit.region = edited;
    const auto changed = fix.session.poll(false, fix.frameSize, {});
    REQUIRE(changed.region);
    CHECK(changed.region->toPixels(1000, 500) == edited.toPixels(1000, 500));
    CHECK(fix.session.faceLocked());
    regionOverlayStubs().borderEdit = {};
    regionOverlayStubs().borderEdit.bindingToggled = true;
    const auto released = fix.session.poll(false, fix.frameSize, {});
    REQUIRE(released.region);
    CHECK(released.region->toPixels(1000, 500) == edited.toPixels(1000, 500));
    CHECK_FALSE(fix.session.faceLocked());
    CHECK(fix.session.attachments().isAttached(Window));
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Window);
    regionOverlayStubs().borderEdit = {};
}

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
    const auto picked = fix.pickWindow();
    (void)fix.session.follow(false, std::nullopt);
    REQUIRE(desktopStubs().windowMotion);
    desktopStubs().windowMotion(WindowMotionSignal::Moved);
    REQUIRE(fix.session.carried());

    RegionSessionOutcome removed;
    SECTION("Full reset")
    {
        removed = fix.session.clear();
        CHECK_FALSE(removed.region);
        CHECK(removed.regionChanged);
    }
    SECTION("Detach action")
    {
        removed = fix.session.detach();
        CHECK(removed.region == picked.region);
    }
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

TEST_CASE("Cancellation with no picker keeps the committed attachment")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    (void)fix.session.follow(false, std::nullopt);
    REQUIRE_FALSE(fix.session.picker().active());
    (void)fix.session.cancel();
    CHECK(fix.session.attachments().isAttached(Window));
    fix.session.syncBorder(false);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == picked.region);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Window);
}

TEST_CASE("Cancelling a picker restores the committed region rather than its preview")
{
    SessionFixture fix;
    const RegionOfInterest committed{20.0, 20.0, 40.0, 40.0};
    (void)fix.session.initializeGlobalRegion(committed);
    RegionPickerMode mode = RegionPickerMode::DrawGlobal;
    SECTION("Draw")
    {
    }
    SECTION("Window")
    {
        mode = RegionPickerMode::AttachWindow;
    }
    SECTION("Face without a detection")
    {
        mode = RegionPickerMode::AttachFace;
    }
    desktopStubs().faceDetectionSupported = mode == RegionPickerMode::AttachFace;
    fix.session.picker().request(mode);
    (void)fix.session.picker().openIfRequested(false);
    regionOverlayStubs().poll.active = true;
    regionOverlayStubs().poll.displayId = Display;
    regionOverlayStubs().poll.preview = RegionOfInterest{60.0, 60.0, 90.0, 90.0};
    const auto preview = fix.session.poll(false, std::nullopt, std::nullopt);
    REQUIRE(preview.region == regionOverlayStubs().poll.preview);
    regionOverlayStubs().poll = {};
    regionOverlayStubs().poll.finished = true;
    const auto restored = fix.session.poll(false, std::nullopt, std::nullopt);
    CHECK(restored.region == committed);
    CHECK(fix.capture.desiredDisplay() == Display);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == committed);
}

TEST_CASE("Unavailable attachment geometry pauses until recovery without detaching")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    (void)fix.session.follow(false, std::nullopt);
    const auto geometry = desktopStubs().windowGeometry;
    desktopStubs().windowGeometry.reset();
    desktopStubs().windowPresence = WindowPresence::Unknown;
    const auto waiting = fix.session.follow(false, std::nullopt);
    CHECK_FALSE(waiting.region);
    CHECK(fix.session.attachments().isAttached(Window));
    CHECK_FALSE(regionOverlayStubs().border);
    desktopStubs().windowGeometry = geometry;
    desktopStubs().foregroundPid = desktopStubs().ownPid;
    desktopStubs().focusedWindow.reset();
    const auto recovered = fix.session.follow(false, std::nullopt);
    CHECK(recovered.region == picked.region);
    CHECK(fix.session.attachments().isAttached(Window));
}

TEST_CASE("Confirmed parent closure preserves the last valid rectangle on its display")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    (void)fix.session.follow(false, std::nullopt);
    desktopStubs().windowGeometry.reset();
    desktopStubs().windowPresence = WindowPresence::Closed;
    desktopStubs().cursorDisplay = 99;
    const auto closed = fix.session.follow(false, std::nullopt);
    CHECK_FALSE(fix.session.attachments().attached());
    CHECK(fix.capture.desiredDisplay() == Display);
    fix.session.syncBorder(false);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == picked.region);
    CHECK(regionOverlayStubs().border->displayId == Display);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Global);
    CHECK(desktopStubs().watchedWindow == 0);
    CHECK(closed.status == "window closed - region kept on its display");
}

TEST_CASE("Stopping all following preserves the latest attachment as a global region")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    (void)fix.session.follow(false, std::nullopt);
    (void)fix.session.detachAll();
    CHECK_FALSE(fix.session.attachments().attached());
    fix.session.syncBorder(false);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == picked.region);
    CHECK(regionOverlayStubs().border->binding == RegionBinding::Global);
}

TEST_CASE("An empty face picker keeps the previously committed attachment")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    desktopStubs().faceDetectionSupported = true;
    fix.session.picker().request(RegionPickerMode::AttachFace);
    (void)fix.session.picker().openIfRequested(false);
    regionOverlayStubs().poll = {};
    regionOverlayStubs().poll.finished = true;
    const auto cancelled = fix.session.poll(false, std::nullopt, std::nullopt);
    CHECK(cancelled.region == picked.region);
    CHECK(fix.session.attachments().isAttached(Window));
    CHECK(fix.capture.desiredDisplay() == Display);
}

TEST_CASE("An explicit cancel restores a preview even before the overlay finishes")
{
    SessionFixture fix;
    const RegionOfInterest committed{20.0, 20.0, 40.0, 40.0};
    (void)fix.session.initializeGlobalRegion(committed);
    fix.session.picker().request(RegionPickerMode::DrawGlobal);
    (void)fix.session.picker().openIfRequested(false);
    regionOverlayStubs().poll.active = true;
    regionOverlayStubs().poll.displayId = Display;
    regionOverlayStubs().poll.preview = RegionOfInterest{60.0, 60.0, 90.0, 90.0};
    (void)fix.session.poll(false, std::nullopt, std::nullopt);
    const auto cancelled = fix.session.cancel();
    CHECK(cancelled.region == committed);
    CHECK_FALSE(fix.session.picker().active());
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == committed);
}

}  // namespace sidescopes
