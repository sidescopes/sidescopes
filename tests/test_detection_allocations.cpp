#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
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
using test::AllocationFailure;
constexpr uint32_t Streamed = 7;
constexpr uint32_t Scanned = 8;
constexpr uint64_t Window = 1;
constexpr AttachWindowRect WindowRect{0.0, 0.0, 200.0, 100.0};

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
        if (!waitUntil([&] { return !picker.scansRunning() && !lock.probeRunning(); })) {
            std::fputs("detached detection did not finish within the cleanup deadline\n", stderr);
            std::abort();
        }
        picker.cancel();
        worker.stop();
        test::desktopStubs().beforeDetection = {};
    }

    void failDetection()
    {
        test::desktopStubs().beforeDetection = [this] {
            const AllocationFailure failure(0);
            detectorScratch.resize(64);
        };
    }

    static void open(RegionPicker& target)
    {
        target.request(RegionPickerMode::AttachFace);
        (void)target.openIfRequested(false);
    }

    void prepareLock()
    {
        worker.startInline();
        mailbox.publish(test::makeSolidFrameBuffer(200, 100, Color{70, 80, 90}, 1));
        worker.pump();
        (void)attach.attach(Window, 20, "Editor", WindowRect, AttachDisplayRect{0, 0, 200, 100}, RegionOfInterest{});
        lock.addLock(Window, face_lock::makeLock(FaceAnchor{100, 50, 20}, LockRect{90, 40, 110, 60}), 0.0, WindowRect);
    }

    FaceLockOutcome updateLock(double now, bool consumeOnly = false)
    {
        AttachDecision decision;
        decision.activeIdentity = Window;
        decision.activeRect = WindowRect;
        return lock.update(decision, worker.latestFrameSize(), Window, std::nullopt, consumeOnly, now);
    }

    static void drainPicker(RegionPicker& target)
    {
        REQUIRE(waitUntil([&] { return !target.scansRunning(); }));
        target.drainFaceScans();
        REQUIRE(test::regionOverlayStubs().deliveredFaces.contains(Scanned));
    }

    void retryLock()
    {
        (void)updateLock(1.1, true);
        CHECK(lock.contains(Window));
        test::desktopStubs().beforeDetection = {};
        (void)updateLock(1.5);
        REQUIRE(waitUntil([&] { return !lock.probeRunning(); }));
        (void)updateLock(1.6, true);
        CHECK(lock.contains(Window));
        // An empty probe resets the confirmation streak. Recovery requires
        // two agreeing sightings, even when they match the original anchor.
        CHECK(lock.hunting());
        (void)updateLock(1.9);
        REQUIRE(waitUntil([&] { return !lock.probeRunning(); }));
        (void)updateLock(2.0, true);
        CHECK(lock.contains(Window));
        CHECK_FALSE(lock.hunting());
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

TEST_CASE("A detached face lock detection allocation failure completes and retries")
{
    Fixture fixture;
    fixture.prepareLock();
    fixture.failDetection();
    (void)fixture.updateLock(1.0);
    REQUIRE(waitUntil([&] { return !fixture.lock.probeRunning(); }));
    CHECK(test::desktopStubs().detectorCall().calls == 1);
    fixture.retryLock();
    CHECK(test::desktopStubs().detectorCall().calls == 3);
}

TEST_CASE("A face lock thread launch allocation failure leaves no phantom running probe")
{
    std::size_t count = 0;
    {
        Fixture measured;
        measured.prepareLock();
        count = countAllocations([&] { (void)measured.updateLock(1.0); });
        REQUIRE(waitUntil([&] { return !measured.lock.probeRunning(); }));
    }
    Fixture fixture;
    fixture.prepareLock();
    const bool threw = failLaunch(count, [&] { (void)fixture.updateLock(1.0); });
    CHECK_FALSE(threw);
    CHECK(test::desktopStubs().detectorCall().calls == 0);
    REQUIRE_FALSE(fixture.lock.probeRunning());
    fixture.retryLock();
    CHECK(test::desktopStubs().detectorCall().calls == 2);
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
