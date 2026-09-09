#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

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

enum class SelectionSource
{
    Global,
    Window,
    Face
};

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
    double captureSeconds = 1000.0;
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

    void publish(uint64_t sequence, Color color = {40, 80, 120})
    {
        captureSeconds += 0.04;
        auto frame = test::makeSolidFrameBuffer(1000, 500, color, sequence);
        frame.stamp = {capture.streamEpoch(), capture.capturedDisplay(), captureSeconds};
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
        const auto result = session.follow(false);
        settings.region = result.region;
        settings.selectionRevision = result.selectionRevision;
        settings.source = AnalysisSettings::Source{capture.streamEpoch(), capture.capturedDisplay()};
        worker.updateSettings(settings);
        return result;
    }

    void selectFace()
    {
        open();
        REQUIRE(regionOverlayStubs().lastDisplays[0].faces.size() == 1);
        REQUIRE(confirm(Display, regionOverlayStubs().lastDisplays[0].faces[0].region).region);
        (void)follow();
        publish(2);
        REQUIRE(follow().region);
    }
};

struct DragSessionFixture : SessionFixture
{
    static constexpr auto Histogram = "org.sidescopes.histogram";
    const AnalysisWorker::FrameSize frameSize{1000, 500, 1000, 500};
    AnalysisSettings settings;
    AnalysisWorker::Output output;
    uint64_t seen = 0;
    std::vector<std::vector<uint8_t>> drawnImages;

    DragSessionFixture()
    {
        desktopStubs().faceDetectionSupported = true;
        desktopStubs().faces = {{200, 100, 100, 100}};
        settings.region = RegionOfInterest{};
        settings.enabledScopes = {Histogram};
        worker.updateSettings(settings);
        worker.startInline();
        publishStaticFrame(1);
    }

    void publishStaticFrame(uint64_t sequence)
    {
        auto frame = test::makeSolidFrameBuffer(1000, 500, {}, sequence);
        frame.stamp = {capture.streamEpoch(), capture.capturedDisplay(), captureSeconds};
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                auto* pixel = frame.data.data() + static_cast<std::size_t>(y) * frame.strideBytes +
                              static_cast<std::size_t>(x) * 4;
                pixel[0] = static_cast<uint8_t>(y / 2);
                pixel[1] = static_cast<uint8_t>(200 - x / 5);
                pixel[2] = static_cast<uint8_t>(x / 4);
            }
        }
        mailbox.publish(std::move(frame));
        worker.pump();
    }

    void apply(const RegionSessionOutcome& outcome)
    {
        settings.selectionRevision = outcome.selectionRevision;
        settings.source = AnalysisSettings::Source{capture.streamEpoch(), capture.capturedDisplay()};
        if (outcome.regionChanged) {
            settings.region = outcome.region;
        }
    }

    void select(SelectionSource binding)
    {
        if (binding == SelectionSource::Global) {
            apply(session.initializeGlobalRegion({20, 20, 30, 40}));
        } else if (binding == SelectionSource::Window) {
            apply(pickWindow());
        } else {
            session.picker().request(RegionPickerMode::AttachFace);
            (void)session.picker().openIfRequested(false);
            REQUIRE(regionOverlayStubs().lastDisplays.at(0).faces.size() == 1);
            regionOverlayStubs().poll.finished = true;
            regionOverlayStubs().poll.displayId = Display;
            regionOverlayStubs().poll.confirmed = regionOverlayStubs().lastDisplays[0].faces[0].region;
            apply(session.poll(false, frameSize, {}));
            regionOverlayStubs().poll = {};
        }
        apply(session.follow(false));
        worker.updateSettings(settings);
        worker.pump();
        REQUIRE(worker.fetchOutput(seen, output, settings.selectionRevision, settings.source));
        REQUIRE(output.frameSequence == 1);
        REQUIRE_FALSE(output.images.at(Histogram).rgba.empty());
    }

    bool step(std::optional<RegionOfInterest> region, bool editing, bool drawing)
    {
        regionOverlayStubs().borderEdit.editing = editing;
        regionOverlayStubs().borderEdit.region = region;
        const auto early = session.pollBorder();
        apply(early);
        if (early.regionChanged) {
            // Native polling consumes a pending geometry delta; this stub
            // deliberately retains it until the test models that consumption.
            regionOverlayStubs().borderEdit.region.reset();
        }
        apply(session.follow(false));
        session.syncBorder(false);
        bool accepted = false;
        if (drawing) {
            accepted = worker.fetchOutput(seen, output, settings.selectionRevision, settings.source);
            if (accepted) {
                const auto& image = output.images.at(Histogram).rgba;
                if (std::find(drawnImages.begin(), drawnImages.end(), image) == drawnImages.end()) {
                    drawnImages.push_back(image);
                }
            }
            // App follows once more after presenting, before its late poll.
            apply(session.follow(false));
        }
        apply(session.poll(false, frameSize, {}));
        regionOverlayStubs().borderEdit.region.reset();
        if (drawing) {
            worker.updateSettings(settings);
            worker.pump();
        }
        return accepted;
    }
};

}  // namespace

TEST_CASE("Border dragging publishes changing static-image scopes before release", "[live-border-drag]")
{
    const auto binding = GENERATE(SelectionSource::Global, SelectionSource::Window, SelectionSource::Face);
    CAPTURE(static_cast<int>(binding));
    DragSessionFixture fix;
    fix.select(binding);
    (void)fix.step({}, true, true);
    fix.drawnImages.clear();
    const auto detectionCalls = desktopStubs().faceDetectionCalls.load();
    for (int move = 0; move < 4; ++move) {
        const double left = 14.0 + move * 6.0;
        // A pointer-only iteration changes the hand's rectangle but defers
        // settings submission. The next drawn iteration fetches a completed
        // earlier pass BEFORE submitting its latest geometry, just like App.
        CHECK_FALSE(fix.step(RegionOfInterest{left, 20, left + 6, 40}, true, false));
        (void)fix.step(RegionOfInterest{left + 2, 20, left + 8, 40}, true, true);
        CHECK(fix.worker.consumedFrameSequence() == 1);
        CHECK(fix.output.frameSequence == 1);
        CHECK_FALSE(fix.worker.held());
    }
    CHECK(fix.drawnImages.size() >= 2);
    CHECK(desktopStubs().faceDetectionCalls.load() == detectionCalls);
    REQUIRE(fix.session.interacting());
    const RegionOfInterest finalCrop{39, 20, 45, 40};
    (void)fix.step(finalCrop, false, true);
    REQUIRE(fix.step({}, false, true));
    REQUIRE(fix.output.region);
    CHECK(fix.output.region->toPixels(1000, 500) == finalCrop.toPixels(1000, 500));
    CHECK(fix.output.frameSequence == 1);
    CHECK_FALSE(fix.session.interacting());
    if (binding == SelectionSource::Face) {
        const auto callsBeforeResume = desktopStubs().faceDetectionCalls.load();
        fix.captureSeconds += 0.04;
        fix.publishStaticFrame(2);
        const auto resumed = fix.session.follow(false);
        REQUIRE(resumed.region);
        CHECK(resumed.region->toPixels(1000, 500) == finalCrop.toPixels(1000, 500));
        CHECK(desktopStubs().faceDetectionCalls.load() == callsBeforeResume);
    }
}

TEST_CASE("Ending a border selection rejects its already completed scope output", "[live-border-drag]")
{
    const auto binding = GENERATE(SelectionSource::Global, SelectionSource::Window, SelectionSource::Face);
    CAPTURE(static_cast<int>(binding));
    DragSessionFixture fix;
    fix.select(binding);
    (void)fix.step({}, true, true);
    (void)fix.step(RegionOfInterest{24, 20, 30, 40}, true, true);
    AnalysisWorker::Output completed;
    uint64_t checked = fix.seen;
    REQUIRE(fix.worker.fetchOutput(checked, completed, fix.settings.selectionRevision));
    const auto priorRevision = fix.settings.selectionRevision;
    const auto priorSeen = fix.seen;
    SECTION("Explicit clear during the gesture")
    {
        fix.apply(fix.session.clear());
    }
    SECTION("Explicit close during the gesture")
    {
        regionOverlayStubs().borderEdit.closed = true;
        fix.apply(fix.session.pollBorder());
        regionOverlayStubs().borderEdit = {};
    }
    SECTION("A new explicit global selection")
    {
        fix.apply(fix.session.clear());
        fix.apply(fix.session.initializeGlobalRegion({60, 20, 75, 40}));
    }
    REQUIRE(fix.settings.selectionRevision > priorRevision);
    CHECK_FALSE(fix.worker.fetchOutput(fix.seen, fix.output, fix.settings.selectionRevision));
    CHECK(fix.seen == priorSeen);
    fix.worker.updateSettings(fix.settings);
    fix.worker.pump();
    if (fix.settings.region) {
        REQUIRE(fix.worker.fetchOutput(fix.seen, fix.output, fix.settings.selectionRevision));
        CHECK(fix.output.region == fix.settings.region);
        CHECK(fix.output.selectionRevision > priorRevision);
    } else {
        CHECK_FALSE(fix.worker.fetchOutput(fix.seen, fix.output, fix.settings.selectionRevision));
    }
}

TEST_CASE("A face-selected border drag performs no further detection", "[live-border-drag]")
{
    DragSessionFixture fix;
    fix.select(SelectionSource::Face);
    (void)fix.step({}, true, true);
    const auto calls = desktopStubs().faceDetectionCalls.load();
    fix.captureSeconds += 0.04;
    fix.publishStaticFrame(2);
    (void)fix.step(RegionOfInterest{24, 20, 30, 40}, true, true);
    CHECK(fix.worker.consumedFrameSequence() == 2);
    CHECK(desktopStubs().faceDetectionCalls.load() == calls);
    CHECK(fix.session.interacting());
}

TEST_CASE("A border drag completed between polls keeps its attached routing", "[live-border-drag]")
{
    const auto binding = GENERATE(SelectionSource::Window, SelectionSource::Face);
    CAPTURE(static_cast<int>(binding));
    DragSessionFixture fix;
    fix.select(binding);
    const RegionOfInterest edited{24, 20, 30, 40};
    // A short down/move/up arrives as geometry with editing already false.
    (void)fix.step(edited, false, true);
    REQUIRE(fix.settings.region);
    CHECK(fix.settings.region->toPixels(1000, 500) == edited.toPixels(1000, 500));
    CHECK(fix.session.attachments().isAttached(Window));
    desktopStubs().windowGeometry->x += 100.0;
    const auto carried = fix.session.follow(false);
    REQUIRE(carried.region);
    CHECK(carried.region->leftPercent == Catch::Approx(edited.leftPercent + 10.0));
    CHECK(carried.region->rightPercent == Catch::Approx(edited.rightPercent + 10.0));
    CHECK(carried.region->topPercent == Catch::Approx(edited.topPercent));
    CHECK(carried.region->bottomPercent == Catch::Approx(edited.bottomPercent));
}

TEST_CASE("A global draw keeps its mode when it matches a picker suggestion", "[picker-mode]")
{
    FaceSessionFixture fix;
    const bool face = GENERATE(false, true);
    const bool sourceMoved = GENERATE(false, true);
    fix.open();
    REQUIRE(regionOverlayStubs().lastDisplays[0].faces.size() == 1);
    const RegionOfInterest drawn =
        face ? regionOverlayStubs().lastDisplays[0].faces[0].region : RegionOfInterest{10, 10, 50, 50};
    // The finishing tool is authoritative even when the picker opened in
    // face mode and retained suggestions with the same coordinates.
    fix.session.picker().request(RegionPickerMode::DrawGlobal);
    (void)fix.session.picker().openIfRequested(false);
    if (sourceMoved) {
        desktopStubs().windowGeometry->x += 100;
    }
    const auto picked = fix.confirm(Display, drawn);
    REQUIRE(picked.region);
    CHECK(*picked.region == drawn);
    CHECK_FALSE(picked.status);
    CHECK_FALSE(fix.session.attachments().attached());
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Global);
}

TEST_CASE("A face suggestion matching its window keeps the suggested crop", "[picker-mode]")
{
    FaceSessionFixture fix;
    desktopStubs().windowGeometry = WindowGeometry{210, 110, 80, 80, false, "Picture"};
    auto& window = desktopStubs().onScreenWindows.front();
    window.x = 210;
    window.y = 110;
    window.width = 80;
    window.height = 80;
    fix.open();
    REQUIRE(regionOverlayStubs().lastDisplays[0].faces.size() == 1);
    const auto suggested = regionOverlayStubs().lastDisplays[0].faces[0].region;
    REQUIRE(fix.session.picker().matchWindowCandidate(Display, suggested));
    const auto picked = fix.confirm(Display, suggested);
    REQUIRE(picked.region);
    CHECK(picked.region->toPixels(1000, 500) == suggested.toPixels(1000, 500));
    CHECK(fix.session.attachments().isAttached(Window));
    (void)fix.follow();
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);
}

TEST_CASE("A non-face draw in face mode remains global even when it matches a window", "[picker-mode]")
{
    FaceSessionFixture fix;
    fix.open();
    const RegionOfInterest drawn{10, 10, 50, 50};
    REQUIRE(fix.session.picker().matchWindowCandidate(Display, drawn));
    REQUIRE_FALSE(fix.session.picker().matchFaceCandidate(Display, drawn));
    const auto picked = fix.confirm(Display, drawn);
    REQUIRE(picked.region);
    CHECK(*picked.region == drawn);
    CHECK_FALSE(fix.session.attachments().attached());
}

TEST_CASE("A window-mode draw matching a face suggestion keeps its attachment", "[picker-mode]")
{
    FaceSessionFixture fix;
    fix.open();
    const auto drawn = regionOverlayStubs().lastDisplays[0].faces[0].region;
    fix.session.picker().request(RegionPickerMode::AttachWindow);
    (void)fix.session.picker().openIfRequested(false);
    const auto picked = fix.confirm(Display, drawn);
    REQUIRE(picked.region);
    CHECK(picked.region->toPixels(1000, 500) == drawn.toPixels(1000, 500));
    CHECK(fix.session.attachments().isAttached(Window));
}

TEST_CASE("A face selection stays fixed as content changes and needs no further detection")
{
    FaceSessionFixture fix;
    fix.selectFace();
    const auto selected = fix.follow();
    REQUIRE(selected.region);
    const auto calls = desktopStubs().faceDetectionCalls.load();
    REQUIRE(calls > 0);
    AnalysisWorker::Output output;
    uint64_t seen = 0;
    for (uint64_t sequence = 3; sequence < 10; ++sequence) {
        desktopStubs().faces = sequence % 2 == 0 ? std::vector<IntRect>{{400, 200, 80, 80}} : std::vector<IntRect>{};
        fix.captureSeconds += 10.0;
        fix.publish(sequence, Color{static_cast<uint8_t>(sequence * 20), 80, 120});
        const auto current = fix.follow();
        CHECK(current.region == selected.region);
        CHECK(current.selectionRevision == selected.selectionRevision);
        CHECK(fix.session.attachments().isAttached(Window));
        REQUIRE(regionOverlayStubs().border);
        CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);
        CHECK(regionOverlayStubs().border->region == selected.region);
        REQUIRE(fix.worker.fetchOutput(seen, output, current.selectionRevision, fix.settings.source));
        CHECK(output.frameSequence == sequence);
        CHECK(output.region == selected.region);
    }
    CHECK(desktopStubs().faceDetectionCalls.load() == calls);
}

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
    CHECK_FALSE(fix.session.attachments().attached());
    CHECK(fix.capture.streamEpoch() == epochBeforeConfirm);
    CHECK(fix.capture.desiredDisplay() == Display);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == committed);
}

TEST_CASE("Snapshot face selection creates a fixed attached crop at either capture scale")
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
    REQUIRE(picked.region);
    REQUIRE(fix.capture.capturedDisplay() == SecondDisplay);
    const auto selectedEpoch = fix.capture.streamEpoch();
    (void)fix.follow();
    fix.worker.pump();
    AnalysisWorker::Output stale;
    uint64_t oldVersion = 0;
    CHECK_FALSE(fix.worker.fetchOutput(oldVersion, stale, fix.settings.selectionRevision, fix.settings.source));
    REQUIRE(desktopStubs().lastDisplayPoint);
    CHECK(desktopStubs().lastDisplayPoint->x == 300.0);
    CHECK(desktopStubs().lastDisplayPoint->y == 150.0);
    CHECK(fix.capture.streamEpoch() == selectedEpoch);
    fix.publish(2);
    const auto first = fix.follow();
    CHECK(first.region == picked.region);
    const auto revision = first.selectionRevision;
    fix.publish(3);
    const auto moved = fix.follow();
    CHECK_FALSE(moved.regionChanged);
    CHECK(moved.selectionRevision == revision);
    REQUIRE(moved.region);
    CHECK(moved.region == picked.region);
    AnalysisWorker::Output output;
    uint64_t seen = 0;
    REQUIRE(fix.worker.fetchOutput(seen, output, revision));
    CHECK(output.frameSequence == 3);
    REQUIRE(output.region);
    // Window-relative storage can round fractional percentages by a few
    // double steps; the sampled pixels must remain identical at both scales.
    CHECK(output.region->toPixels(1000, 500) == moved.region->toPixels(1000, 500));
    CHECK(output.region->toPixels(2000, 1000) == moved.region->toPixels(2000, 1000));
    CHECK(output.frameStamp.displayId == SecondDisplay);
    CHECK(output.frameStamp.captureEpoch == selectedEpoch);
}

TEST_CASE("A face pick is clamped to its parent window like any attached crop")
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
    CHECK(fix.session.attachments().isAttached(Window));
    REQUIRE(ordinary.region);
    CHECK(ordinary.region->leftPercent >= 24.0);
    CHECK(ordinary.region->rightPercent <= 26.0);
}

TEST_CASE("A face selection survives animated minimization and an empty cancelled picker")
{
    FaceSessionFixture fix;
    double now = 1.0;
    desktopStubs().clock = [&] { return now; };
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
    now = 2.05;
    desktopStubs().windowGeometry->minimized = true;
    CHECK_FALSE(fix.follow().region);
    CHECK_FALSE(regionOverlayStubs().border);
    desktopStubs().onScreenWindows.clear();
    desktopStubs().faces.clear();
    fix.open();
    REQUIRE(regionOverlayStubs().lastDisplays[0].faces.empty());
    (void)fix.session.cancel();
    CHECK(fix.session.attachments().isAttached(Window));
    CHECK_FALSE(regionOverlayStubs().border);

    now = 5.0;
    desktopStubs().windowGeometry->minimized = false;
    CHECK_FALSE(fix.follow().region);
    CHECK_FALSE(fix.follow().region);  // same UI frame is not a settled window
    now = 5.05;
    desktopStubs().windowGeometry = original;
    CHECK_FALSE(fix.follow().region);
    now = 5.3;
    const auto restored = fix.follow();
    REQUIRE(restored.region);
    CHECK(restored.region->toPixels(1000, 500) == accepted.region->toPixels(1000, 500));
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);
    fix.publish(3);
    const auto resumed = fix.follow();
    REQUIRE(resumed.region);
    CHECK(resumed.region->toPixels(1000, 500) == accepted.region->toPixels(1000, 500));
    desktopStubs().clock = {};
}

TEST_CASE("Cancelling an empty face picker preserves attachment across paused capture streams")
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
    fix.open();
    const auto picked = fix.confirm(Display, regionOverlayStubs().lastDisplays[0].faces[0].region);
    REQUIRE(picked.region);
    (void)fix.follow();
    fix.publish(2);
    const auto accepted = fix.follow();
    REQUIRE(accepted.region);
    const auto firstEpoch = fix.capture.streamEpoch();
    const int calls = desktopStubs().faceDetectionCalls.load();

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
    const auto pickerEpoch = fix.capture.streamEpoch();
    regionOverlayStubs().poll = {};
    (void)fix.session.cancel();
    fix.capture.suspend("waiting for a region source");
    fix.worker.releaseFrame();

    desktopStubs().windowGeometry->minimized = false;
    now += 1.0;
    CHECK_FALSE(fix.follow().region);
    now += 0.3;
    REQUIRE(fix.follow().region);
    fix.capture.resume();
    REQUIRE(fix.capture.streamEpoch() > pickerEpoch);
    (void)fix.follow();
    auto late = test::makeSolidFrameBuffer(1000, 500, Color{40, 80, 120}, 3);
    late.stamp = {firstEpoch, Display, frameClockSeconds()};
    fix.mailbox.publish(std::move(late));
    fix.worker.pump();
    CHECK(desktopStubs().faceDetectionCalls.load() == calls);
    fix.publish(1);  // fresh stream numbering may restart
    const auto resumed = fix.follow();
    REQUIRE(resumed.region);
    CHECK(resumed.region->toPixels(1000, 500) == accepted.region->toPixels(1000, 500));
    CHECK(desktopStubs().faceDetectionCalls.load() == calls);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);
    desktopStubs().clock = {};
}

TEST_CASE("Closure during a minimize animation keeps the last settled attached crop")
{
    SessionFixture fix;
    double now = 1.0;
    desktopStubs().clock = [&] { return now; };
    const auto picked = fix.pickWindow();
    REQUIRE(picked.region);
    (void)fix.session.follow(false);
    now = 2.0;
    desktopStubs().windowGeometry = WindowGeometry{700, 400, 100, 50, false, "Picture"};
    (void)fix.session.follow(false);
    desktopStubs().windowGeometry.reset();
    desktopStubs().windowPresence = WindowPresence::Closed;
    const auto closed = fix.session.follow(false);
    CHECK(closed.region == picked.region);
    CHECK_FALSE(fix.session.attachments().attached());
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Global);
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
    CHECK(fix.session.attachments().isAttached(Window));
    // Neither queued edit may replace the last settled attached crop.
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
    desktopStubs().clock = {};
}

TEST_CASE("Editing a face-selected region and detaching preserves its current rectangle")
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
    regionOverlayStubs().borderEdit = {};
    regionOverlayStubs().borderEdit.bindingToggled = true;
    const auto released = fix.session.poll(false, fix.frameSize, {});
    REQUIRE(released.region);
    CHECK(released.region->toPixels(1000, 500) == edited.toPixels(1000, 500));
    CHECK_FALSE(fix.session.attachments().isAttached(Window));
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Global);
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

    (void)fix.session.follow(false);
    CHECK(desktopStubs().watchedWindow == Window);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);
    CHECK(regionOverlayStubs().border->label == "Picture");

    desktopStubs().foregroundPid = desktopStubs().ownPid;
    desktopStubs().focusedWindow.reset();
    (void)fix.session.follow(false);
    CHECK(desktopStubs().watchedWindow == Window);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);

    desktopStubs().windowGeometry->x += 100.0;
    const auto moved = fix.session.follow(false);
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
    (void)fix.session.follow(false);
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
    CHECK(regionOverlayStubs().border->kind == RegionKind::Global);
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
    (void)fix.session.follow(false);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);
    CHECK(regionOverlayStubs().border->region == selected);

    regionOverlayStubs().borderEdit.bindingToggled = true;
    (void)fix.session.poll(false, std::nullopt, std::nullopt);
    CHECK_FALSE(fix.session.attachments().attached());
    CHECK(desktopStubs().watchedWindow == 0);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Global);
    CHECK(regionOverlayStubs().border->region == selected);
}

TEST_CASE("A region session releases native callbacks when destroyed")
{
    {
        SessionFixture fix;
        (void)fix.pickWindow();
        (void)fix.session.follow(false);
        REQUIRE(desktopStubs().windowMotion);
    }
    CHECK(desktopStubs().watchedWindow == 0);
    CHECK_FALSE(desktopStubs().windowMotion);
}

TEST_CASE("Closing a region removes its selection without recreating the startup region")
{
    SessionFixture fix;
    SECTION("Global startup region")
    {
        (void)fix.session.initializeGlobalRegion({20, 20, 60, 60});
    }
    SECTION("Attached region")
    {
        REQUIRE(fix.pickWindow().region);
        (void)fix.session.follow(false);
    }
    regionOverlayStubs().borderEdit.closed = true;
    const auto closed = fix.session.pollBorder();
    regionOverlayStubs().borderEdit = {};
    CHECK(closed.regionChanged);
    CHECK_FALSE(closed.region);
    CHECK_FALSE(fix.session.attachments().attached());
    CHECK_FALSE(regionOverlayStubs().border);
    CHECK_FALSE(fix.session.follow(false).region);
    CHECK_FALSE(fix.session.poll(false, {}, {}).region);
}

TEST_CASE("Closing one region preserves other windows while explicit clear removes them all")
{
    SessionFixture fix;
    const auto first = fix.pickWindow();
    REQUIRE(first.region);
    constexpr uint64_t SecondWindow = Window + 1;
    desktopStubs().onScreenWindows.front().windowIdentity = SecondWindow;
    desktopStubs().focusedWindow = SecondWindow;
    REQUIRE(fix.pickWindow().region);
    (void)fix.session.follow(false);
    REQUIRE(fix.session.attachments().attachedCount() == 2);
    SECTION("Close the current border")
    {
        regionOverlayStubs().borderEdit.closed = true;
        CHECK_FALSE(fix.session.pollBorder().region);
        regionOverlayStubs().borderEdit = {};
        CHECK(fix.session.attachments().isAttached(Window));
        CHECK_FALSE(fix.session.attachments().isAttached(SecondWindow));
        desktopStubs().focusedWindow = Window;
        CHECK(fix.session.follow(false).region == first.region);
    }
    SECTION("Clear all selections")
    {
        const auto cleared = fix.session.clear();
        CHECK(cleared.regionChanged);
        CHECK_FALSE(cleared.region);
        CHECK_FALSE(fix.session.attachments().attached());
        CHECK_FALSE(fix.session.follow(false).region);
    }
}

TEST_CASE("Cancellation with no picker keeps the committed attachment")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    (void)fix.session.follow(false);
    REQUIRE_FALSE(fix.session.picker().active());
    (void)fix.session.cancel();
    CHECK(fix.session.attachments().isAttached(Window));
    fix.session.syncBorder(false);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == picked.region);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Attached);
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
    (void)fix.session.follow(false);
    const auto geometry = desktopStubs().windowGeometry;
    desktopStubs().windowGeometry.reset();
    desktopStubs().windowPresence = WindowPresence::Unknown;
    const auto waiting = fix.session.follow(false);
    CHECK_FALSE(waiting.region);
    CHECK(fix.session.attachments().isAttached(Window));
    CHECK_FALSE(regionOverlayStubs().border);
    desktopStubs().windowGeometry = geometry;
    desktopStubs().foregroundPid = desktopStubs().ownPid;
    desktopStubs().focusedWindow.reset();
    const auto recovered = fix.session.follow(false);
    CHECK(recovered.region == picked.region);
    CHECK(fix.session.attachments().isAttached(Window));
}

TEST_CASE("Confirmed parent closure preserves the last valid rectangle on its display")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    (void)fix.session.follow(false);
    desktopStubs().windowGeometry.reset();
    desktopStubs().windowPresence = WindowPresence::Closed;
    desktopStubs().cursorDisplay = 99;
    const auto closed = fix.session.follow(false);
    CHECK_FALSE(fix.session.attachments().attached());
    CHECK(fix.capture.desiredDisplay() == Display);
    fix.session.syncBorder(false);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == picked.region);
    CHECK(regionOverlayStubs().border->displayId == Display);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Global);
    CHECK(desktopStubs().watchedWindow == 0);
    CHECK(closed.status == "window closed - region kept on its display");
}

TEST_CASE("Stopping all following preserves the latest attachment as a global region")
{
    SessionFixture fix;
    const auto picked = fix.pickWindow();
    (void)fix.session.follow(false);
    (void)fix.session.detachAll();
    CHECK_FALSE(fix.session.attachments().attached());
    fix.session.syncBorder(false);
    REQUIRE(regionOverlayStubs().border);
    CHECK(regionOverlayStubs().border->region == picked.region);
    CHECK(regionOverlayStubs().border->kind == RegionKind::Global);
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
