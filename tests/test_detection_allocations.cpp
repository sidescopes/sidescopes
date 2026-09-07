#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "allocation_failure.h"
#include "app/capture_controller.h"
#include "app/face_lock_controller.h"
#include "app/region_picker.h"
#include "app/region_session.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "region_overlay_stubs.h"
#include "test_frame.h"

namespace sidescopes {
namespace {
using Catch::Matchers::WithinULP;
using test::AllocationFailure;
constexpr uint32_t Streamed = 7;
constexpr uint32_t Scanned = 8;
constexpr uint64_t Window = 1;
constexpr AttachWindowRect WindowRect{0.0, 0.0, 200.0, 100.0};

void checkSameCrop(const RegionOfInterest& actual, const RegionOfInterest& expected)
{
    CHECK_THAT(actual.leftPercent, WithinULP(expected.leftPercent, 4));
    CHECK_THAT(actual.topPercent, WithinULP(expected.topPercent, 4));
    CHECK_THAT(actual.rightPercent, WithinULP(expected.rightPercent, 4));
    CHECK_THAT(actual.bottomPercent, WithinULP(expected.bottomPercent, 4));
    const auto actualPixels = actual.toPixels(200, 100);
    const auto expectedPixels = expected.toPixels(200, 100);
    CAPTURE(actualPixels.x, actualPixels.y, actualPixels.width, actualPixels.height);
    CAPTURE(expectedPixels.x, expectedPixels.y, expectedPixels.width, expectedPixels.height);
    CHECK(actualPixels == expectedPixels);
}

template <typename Ready>
bool waitUntil(Ready ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ready();
}

struct Fixture
{
    test::FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    AttachController attach;
    RegionPicker picker{capture, worker, source};
    FaceLockController lock{attach, worker, capture};
    std::vector<uint8_t> detectorScratch;
    AnalysisSettings settings;
    bool failureInjected = false;

    Fixture()
    {
        auto& desktop = test::desktopStubs();
        desktop.reset();
        test::regionOverlayStubs().reset();
        source.targets = {test::makeTarget(Streamed, "Streamed"), test::makeTarget(Scanned, "Other")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(Streamed);
        REQUIRE(capture.start());
        desktop.faceDetectionSupported = true;
        desktop.displayGeometry = DisplayGeometry{0.0, 0.0, 200.0, 100.0};
        desktop.faces.push_back({40, 40, 20, 20});
        desktop.displayImage = CapturedImage{PixelStorage(std::size_t{200} * 100 * 4, 8), 200, 100};
    }

    ~Fixture()
    {
        // A regression must fail its subprocess within a bound, not hang the
        // suite or free storage a real detached thread might still be using.
        if (!waitUntil([&] { return !picker.scansRunning(); })) {
            std::fputs("detached detection did not finish within the cleanup deadline\n", stderr);
            std::abort();
        }
        picker.cancel();
        worker.stop();
        test::desktopStubs().beforeDetection = {};
        test::desktopStubs().beforeSessionCreation = {};
    }

    void failDetection()
    {
        test::desktopStubs().beforeDetection = [this] {
            const AllocationFailure failure(0);
            try {
                detectorScratch.resize(64);
            } catch (const std::bad_alloc&) {
                failureInjected = failure.failures() == 1;
                throw;
            }
        };
    }

    static void open(RegionPicker& target)
    {
        target.request(RegionPickerMode::AttachFace);
        (void)target.openIfRequested(false);
    }

    void prepareLock()
    {
        (void)attach.attach(Window, 20, "Editor", WindowRect, AttachDisplayRect{0, 0, 200, 100}, RegionOfInterest{});
        lock.addLock(Window, face_lock::makeLock(FaceAnchor{100, 50, 40}, LockRect{90, 40, 110, 60}), WindowRect);
        test::desktopStubs().faces.assign(1, IntRect{80, 30, 40, 40});
        (void)updateLock();
        settings.region = RegionOfInterest{45, 40, 55, 60};
        settings.selectionRevision = lock.selectionRevision();
        settings.enabledScopes = {"org.sidescopes.histogram"};
        worker.updateSettings(settings);
        worker.startInline();
        worker.pump();
    }

    FaceLockOutcome updateLock(double now = frameClockSeconds())
    {
        AttachDecision decision;
        decision.activeIdentity = Window;
        decision.activeRect = WindowRect;
        return lock.update(decision, AnalysisWorker::FrameSize{200, 100, 200, 100}, false, now);
    }

    static void drainPicker(RegionPicker& target)
    {
        REQUIRE(waitUntil([&] { return !target.scansRunning(); }));
        target.drainFaceScans();
        REQUIRE(test::regionOverlayStubs().deliveredFaces.contains(Scanned));
    }

    FaceLockOutcome advanceLock(uint64_t sequence)
    {
        (void)updateLock();
        settings.selectionRevision = lock.selectionRevision();
        worker.updateSettings(settings);
        auto frame = test::makeSolidFrameBuffer(200, 100, Color{70, 80, 90}, sequence);
        frame.stamp = {capture.streamEpoch(), Streamed, frameClockSeconds()};
        mailbox.publish(std::move(frame));
        worker.pump();
        REQUIRE(worker.consumedFrameSequence() == sequence);
        return updateLock();
    }

    void retryLock()
    {
        test::desktopStubs().beforeDetection = {};
        test::desktopStubs().beforeSessionCreation = {};
        const auto recovered = advanceLock(2);
        REQUIRE(recovered.applyRegion);
        CHECK_FALSE(recovered.lostLock);
        CHECK(lock.contains(Window));
        CHECK(attach.isAttached(Window));
        AnalysisWorker::Output output;
        uint64_t seen = 0;
        REQUIRE(worker.fetchOutput(seen, output, lock.selectionRevision()));
        CHECK(output.frameSequence == 2u);
        REQUIRE(output.region);
        checkSameCrop(*output.region, *recovered.applyRegion);
        REQUIRE(output.images.contains("org.sidescopes.histogram"));
        CHECK_FALSE(output.images.at("org.sidescopes.histogram").rgba.empty());
    }
};

template <typename Operation>
std::size_t countAllocations(Operation operation)
{
    AllocationFailure counter(AllocationFailure::CountOnly);
    operation();
    counter.disarm();
    const auto count = counter.attempts();
    REQUIRE(count > 0);
    REQUIRE(count < 128);
    return count;
}

template <typename Operation>
bool failLaunch(std::size_t count, Operation operation)
{
    // The final caller allocation is std::thread's launch state. Detached
    // detector allocations run on another thread and are excluded by the probe.
    AllocationFailure failure(count - 1);
    bool threw = false;
    try {
        operation();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    failure.disarm();
    REQUIRE(failure.failures() == 1);
    return threw;
}
}  // namespace

TEST_CASE("A detached picker detection allocation failure completes empty and retries")
{
    Fixture fixture;
    RegionSession session(fixture.capture, fixture.worker, fixture.source);
    fixture.failDetection();
    Fixture::open(session.picker());
    Fixture::drainPicker(session.picker());
    CHECK(test::desktopStubs().detectorCall().calls == 1);
    CHECK(test::regionOverlayStubs().deliveredFaces.at(Scanned).empty());
    CHECK_FALSE(session.backgroundWorkRunning());

    session.picker().cancel();
    test::desktopStubs().beforeDetection = {};
    test::regionOverlayStubs().deliveredFaces.clear();
    Fixture::open(session.picker());
    Fixture::drainPicker(session.picker());
    CHECK(test::regionOverlayStubs().deliveredFaces.at(Scanned).size() == 1);
    session.shutdown();
    CHECK_FALSE(session.backgroundWorkRunning());
    CHECK_FALSE(test::regionOverlayStubs().pickActive);
}

TEST_CASE("A picker thread launch allocation failure leaves no phantom running scan")
{
    std::size_t count = 0;
    {
        Fixture measured;
        count = countAllocations([&] { Fixture::open(measured.picker); });
        Fixture::drainPicker(measured.picker);
    }
    Fixture fixture;
    const bool threw = failLaunch(count, [&] { Fixture::open(fixture.picker); });
    CHECK_FALSE(threw);
    CHECK(test::desktopStubs().detectorCall().calls == 0);
    REQUIRE_FALSE(fixture.picker.scansRunning());
    fixture.picker.drainFaceScans();
    REQUIRE(test::regionOverlayStubs().deliveredFaces.contains(Scanned));
    CHECK(test::regionOverlayStubs().deliveredFaces.at(Scanned).empty());
    fixture.picker.cancel();
    test::regionOverlayStubs().deliveredFaces.clear();
    Fixture::open(fixture.picker);
    Fixture::drainPicker(fixture.picker);
    CHECK(test::regionOverlayStubs().deliveredFaces.at(Scanned).size() == 1);
}

TEST_CASE("A same-frame face detection allocation failure is contained and the next frame recovers")
{
    Fixture fixture;
    fixture.prepareLock();
    fixture.failDetection();
    FaceLockOutcome failed;
    REQUIRE_NOTHROW(failed = fixture.advanceLock(1));
    CHECK(fixture.failureInjected);
    CHECK_FALSE(failed.applyRegion);
    CHECK_FALSE(failed.lostLock);
    CHECK(test::desktopStubs().detectorCall().calls == 1);
    fixture.retryLock();
    CHECK(test::desktopStubs().detectorCall().calls == 2);
}

TEST_CASE("A native face session construction allocation failure can retry on fresh pixels")
{
    Fixture fixture;
    fixture.prepareLock();
    fixture.failDetection();
    test::desktopStubs().beforeSessionCreation = std::move(test::desktopStubs().beforeDetection);
    FaceLockOutcome failed;
    REQUIRE_NOTHROW(failed = fixture.advanceLock(1));
    CHECK(fixture.failureInjected);
    CHECK_FALSE(failed.applyRegion);
    CHECK_FALSE(failed.lostLock);
    CHECK(test::desktopStubs().detectorCall().calls == 0);
    fixture.retryLock();
    CHECK(test::desktopStubs().detectorCall().calls == 1);
}

TEST_CASE("A failed selection snapshot keeps an ordinary crop until tracking can be configured again")
{
    Fixture fixture;
    fixture.prepareLock();
    REQUIRE(fixture.advanceLock(1).applyRegion);
    AllocationFailure failure(0);
    bool threw = false;
    try {
        fixture.lock.addLock(Window, face_lock::makeLock({100, 50, 40}, {90, 40, 110, 60}), WindowRect);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    failure.disarm();
    REQUIRE(failure.failures() == 1);
    CHECK_FALSE(threw);
    fixture.settings.selectionRevision = fixture.lock.selectionRevision();
    fixture.worker.updateSettings(fixture.settings);
    fixture.worker.pump();
    AnalysisWorker::Output output;
    uint64_t seen = 0;
    REQUIRE(fixture.worker.fetchOutput(seen, output, fixture.lock.selectionRevision()));
    CHECK(output.region == fixture.settings.region);
    CHECK(fixture.attach.isAttached(Window));
    CHECK(test::desktopStubs().detectorCall().calls == 1);
    fixture.retryLock();
    CHECK(test::desktopStubs().detectorCall().calls == 2);
}

TEST_CASE("Allocation withdrawal remains fetchable for the active selection")
{
    Fixture fixture;
    fixture.prepareLock();
    REQUIRE(fixture.advanceLock(1).applyRegion);
    fixture.settings.sampleThinning = 2;
    fixture.worker.updateSettings(fixture.settings);
    AllocationFailure failure(0);
    bool threw = false;
    try {
        fixture.worker.pump();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    failure.disarm();
    REQUIRE(failure.failures() == 1);
    CHECK_FALSE(threw);
    AnalysisWorker::Output output;
    uint64_t seen = 0;
    REQUIRE(fixture.worker.fetchOutput(seen, output, fixture.lock.selectionRevision()));
    CHECK(output.images.empty());
    CHECK(output.outlines.empty());
    CHECK(output.frameSequence == 0u);
    CHECK_FALSE(output.region);
    CHECK(output.selectionRevision == fixture.lock.selectionRevision());
    fixture.worker.pump();
    REQUIRE(fixture.worker.fetchOutput(seen, output, fixture.lock.selectionRevision()));
    CHECK_FALSE(output.images.empty());
    CHECK(output.frameSequence == 1u);
}

TEST_CASE("A capture status allocation failure still records the stopped stream")
{
    Fixture fixture;
    REQUIRE_FALSE(fixture.capture.dead());
    const std::string message(512, 'x');
    AllocationFailure failure(0);
    bool threw = false;
    try {
        fixture.source.fireStatus(message);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    failure.disarm();
    CHECK(failure.failures() == 1);
    CHECK(threw);
    CHECK(fixture.capture.dead());
    REQUIRE(fixture.capture.start());
    CHECK_FALSE(fixture.capture.dead());
}

}  // namespace sidescopes
