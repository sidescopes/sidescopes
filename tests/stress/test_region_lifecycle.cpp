#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <random>
#include <thread>

#include "app/capture_controller.h"
#include "app/region_session.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "region_overlay_stubs.h"
#include "stress_config.h"
#include "test_frame.h"

namespace sidescopes {
namespace {

using namespace std::chrono_literals;
constexpr uint32_t Display = 7;
constexpr uint64_t Window = 42;

// The OS seam is synthetic; the producer, mailbox, analysis worker, scope
// modules and application controllers run their actual threaded paths.
class ThreadedCapture : public test::FakeCaptureSource
{
public:
    ~ThreadedCapture() override
    {
        ThreadedCapture::stop();
    }

    bool start(const CaptureTarget& target, int framesPerSecond, FrameMailbox& mailbox) override
    {
        if (!test::FakeCaptureSource::start(target, framesPerSecond, mailbox)) {
            return false;
        }
        m_stop.store(false);
        m_thread = std::thread([this, &mailbox] {
            while (!m_stop.load()) {
                const uint64_t sequence = ++published;
                const uint8_t code = static_cast<uint8_t>(sequence % 200 + 30);
                mailbox.publish(test::makeSolidFrameBuffer(128, 96, Color{code, 80, 140}, sequence));
                std::this_thread::sleep_for(5ms);
            }
        });
        return true;
    }

    void stop() override
    {
        m_stop.store(true);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        test::FakeCaptureSource::stop();
    }

    std::atomic<uint64_t> published{0};

private:
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

struct ResetDesktop
{
    ResetDesktop()
    {
        test::desktopStubs().reset();
        test::regionOverlayStubs().reset();
        test::desktopStubs().clock = [] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        };
    }
};

struct SessionFixture : ResetDesktop
{
    FrameMailbox mailbox;
    ThreadedCapture source;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    RegionSession session{capture, worker, source};
    AnalysisSettings settings;

    SessionFixture()
    {
        auto& desktop = test::desktopStubs();
        source.targets = {test::makeTarget(Display, "Display")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(Display);
        REQUIRE(capture.start());
        desktop.displayGeometry = DisplayGeometry{0, 0, 1000, 500};
        desktop.cursorDisplay = Display;
        restoreWindow();
        settings.enabledScopes = {"org.sidescopes.vectorscope", "org.sidescopes.waveform"};
        for (const auto& id : settings.enabledScopes) {
            settings.imageSizes[id] = {64, 64};
        }
        worker.updateSettings(settings);
        worker.start();
    }

    ~SessionFixture()
    {
        session.shutdown();
        source.stop();
        worker.stop();
    }

    static void restoreWindow()
    {
        auto& desktop = test::desktopStubs();
        desktop.windowGeometry = WindowGeometry{100, 50, 400, 200, false, "Picture"};
        DesktopWindow window;
        window.windowIdentity = Window;
        window.ownerPid = 420;
        window.application = "Editor";
        window.x = 100;
        window.y = 50;
        window.width = 400;
        window.height = 200;
        desktop.onScreenWindows = {window};
        desktop.foregroundPid = 420;
        desktop.focusedWindow = Window;
    }

    void apply(const RegionSessionOutcome& outcome)
    {
        if (outcome.regionChanged) {
            settings.region = outcome.region;
            worker.updateSettings(settings);
        }
    }

    void open(RegionPickerMode mode)
    {
        session.picker().request(mode);
        apply(session.poll(false, worker.latestFrameSize(), std::nullopt));
        REQUIRE(session.picker().active());
        REQUIRE(test::regionOverlayStubs().pickActive);
    }

    void confirm(RegionOfInterest region)
    {
        auto& overlays = test::regionOverlayStubs();
        overlays.poll.finished = true;
        overlays.poll.displayId = Display;
        overlays.poll.confirmed = region;
        apply(session.poll(false, worker.latestFrameSize(), std::nullopt));
        overlays.poll = {};
        REQUIRE_FALSE(session.picker().active());
        REQUIRE_FALSE(overlays.pickActive);
        REQUIRE(settings.region);
    }
};

void exerciseAttachment(SessionFixture& fix, std::mt19937& random)
{
    auto& desktop = test::desktopStubs();
    SessionFixture::restoreWindow();
    fix.open(RegionPickerMode::AttachWindow);
    fix.confirm({10, 10, 50, 50});
    fix.apply(fix.session.follow(false, fix.worker.latestFrameSize()));
    REQUIRE(fix.session.attachments().isAttached(Window));
    REQUIRE(desktop.windowMotion);
    REQUIRE(desktop.watchedWindow == Window);

    desktop.windowMotion(WindowMotionSignal::GripDown);
    desktop.windowMotion(WindowMotionSignal::MotionImminent);
    desktop.windowGeometry->x += static_cast<double>(random() % 150);
    desktop.windowGeometry->width += static_cast<double>(random() % 150);
    fix.apply(fix.session.follow(false, fix.worker.latestFrameSize()));
    REQUIRE(fix.session.carried());
    REQUIRE_FALSE(test::regionOverlayStubs().border);
    desktop.windowMotion(WindowMotionSignal::GripUp);
    desktop.foregroundPid = desktop.ownPid;
    desktop.focusedWindow.reset();
    fix.apply(fix.session.follow(false, fix.worker.latestFrameSize()));
    REQUIRE(desktop.watchedWindow == Window);

    if (random() % 2 == 0) {
        desktop.windowGeometry.reset();
        fix.apply(fix.session.follow(false, fix.worker.latestFrameSize()));
    } else {
        fix.apply(fix.session.detach());
    }
    REQUIRE_FALSE(fix.session.attachments().attached());
    REQUIRE_FALSE(fix.session.carried());
    REQUIRE(desktop.watchedWindow == 0);
    REQUIRE_FALSE(desktop.windowMotion);
}

void exerciseCapturePause(SessionFixture& fix)
{
    fix.capture.suspend("synthetic lifecycle pause");
    const int starts = fix.source.startCount;
    fix.capture.markStale();
    fix.capture.service(10.0);
    REQUIRE(fix.capture.suspended());
    REQUIRE(fix.source.startCount == starts);
    fix.capture.resume();
    REQUIRE_FALSE(fix.capture.dead());
    REQUIRE(fix.source.startCount == starts + 1);
    fix.capture.service(10.1);
    REQUIRE(fix.source.startCount == starts + 1);
}

// Leaves the streamed display empty so only the second display starts a
// detached detector. All detector inputs stay immutable until the session drains.
struct DetachedScanFixture : ResetDesktop
{
    test::FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};

    DetachedScanFixture()
    {
        source.targets = {test::makeTarget(Display, "Streamed"), test::makeTarget(Display + 1, "Other")};
        REQUIRE(capture.requestPermission());
        capture.requestDisplay(Display);
        REQUIRE(capture.start());
        auto& desktop = test::desktopStubs();
        desktop.displayGeometry = DisplayGeometry{0, 0, 1000, 500};
        desktop.faceDetectionSupported = true;
        CapturedImage captured;
        captured.width = 128;
        captured.height = 96;
        captured.bgra.resize(std::size_t{128} * 96 * 4);
        desktop.displayImage = std::move(captured);
    }
};

template <typename Predicate>
bool waitUntil(Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void verifyAnalysisProgress(SessionFixture& fix, uint64_t& lastProcessed)
{
    uint64_t seen = 0;
    AnalysisWorker::Output output;
    REQUIRE(waitUntil([&] {
        return fix.worker.fetchOutput(seen, output) && output.framesProcessed > lastProcessed &&
               output.images.size() == 2;
    }));
    for (const auto& [id, image] : output.images) {
        CAPTURE(id);
        REQUIRE(image.width > 0);
        REQUIRE(image.height > 0);
        REQUIRE(image.rgba.size() == static_cast<size_t>(image.width) * image.height * 4);
        REQUIRE(image.sequence > 0);
    }
    lastProcessed = output.framesProcessed;
}

struct ReleaseScan
{
    std::promise<void>& signal;
    bool released = false;

    ~ReleaseScan()
    {
        open();
    }

    void open()
    {
        if (!released) {
            signal.set_value();
            released = true;
        }
    }
};

}  // namespace

TEST_CASE("Seeded region lifecycles survive live synthetic frame delivery", "[stress]")
{
    const uint32_t seed = test::stressSetting("SIDESCOPES_STRESS_SEED", 1592598566, UINT32_MAX);
    const uint32_t cycles = test::stressSetting("SIDESCOPES_STRESS_CYCLES", 64, 100000);
    std::mt19937 random(seed);
    SessionFixture fix;
    uint64_t lastProcessed = 0;
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        CAPTURE(seed, cycle);
        fix.apply(fix.session.clear());
        fix.open(RegionPickerMode::DrawGlobal);
        fix.session.picker().cancel();
        REQUIRE_FALSE(test::regionOverlayStubs().pickActive);
        fix.open(RegionPickerMode::DrawGlobal);
        const double left = 55.0 + static_cast<double>(random() % 10);
        fix.confirm({left, 20, left + 20, 65});
        REQUIRE_FALSE(fix.session.attachments().attached());
        verifyAnalysisProgress(fix, lastProcessed);
        exerciseAttachment(fix, random);
        exerciseCapturePause(fix);
        fix.apply(fix.session.clear());
        REQUIRE_FALSE(fix.settings.region);
    }
    fix.session.shutdown();
    fix.session.shutdown();
    REQUIRE_FALSE(fix.session.backgroundWorkRunning());
    REQUIRE_FALSE(test::regionOverlayStubs().pickActive);
    REQUIRE_FALSE(test::desktopStubs().windowMotion);
    REQUIRE(fix.source.published.load() > cycles);
    std::fprintf(stderr, "lifecycle seed=%u cycles=%u capture_starts=%d frames_published=%llu\n", seed, cycles,
                 fix.source.startCount, static_cast<unsigned long long>(fix.source.published.load()));
}

TEST_CASE("Shutdown waits for a cancelled picker scan and discards its stale result", "[stress]")
{
    const uint32_t cycles = test::stressSetting("SIDESCOPES_STRESS_CYCLES", 64, 100000);
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        CAPTURE(cycle);
        DetachedScanFixture fix;
        auto& desktop = test::desktopStubs();

        std::promise<void> entered;
        std::promise<void> release;
        auto released = release.get_future().share();
        desktop.beforeDetection = [&] {
            entered.set_value();
            released.wait();
        };
        RegionSession session(fix.capture, fix.worker, fix.source);
        ReleaseScan releaseOnExit{release};
        session.picker().request(RegionPickerMode::AttachFace);
        (void)session.picker().openIfRequested(false);
        const bool scanEntered = entered.get_future().wait_for(5s) == std::future_status::ready;
        // Release before any assertion can unwind the session's draining
        // destructor. The headless owner thread alone touches session state.
        std::promise<void> shutdownStarted;
        auto stopped = std::async(std::launch::async, [&] {
            shutdownStarted.set_value();
            session.shutdown();
        });
        shutdownStarted.get_future().wait();
        const bool shutdownWaited = stopped.wait_for(5ms) == std::future_status::timeout;
        releaseOnExit.open();
        stopped.get();
        REQUIRE(scanEntered);
        REQUIRE(shutdownWaited);
        REQUIRE_FALSE(session.backgroundWorkRunning());
        REQUIRE_FALSE(test::regionOverlayStubs().pickActive);
        session.picker().drainFaceScans();
        REQUIRE(test::regionOverlayStubs().deliveredFaces.empty());
        desktop.beforeDetection = {};
    }
    std::fprintf(stderr, "detached_scan_shutdown cycles=%u\n", cycles);
}

TEST_CASE("Reopened picker rejects a cancelled scan after its replacement scan completes", "[stress]")
{
    const uint32_t cycles = test::stressSetting("SIDESCOPES_STRESS_CYCLES", 64, 100000);
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        CAPTURE(cycle);
        DetachedScanFixture fix;
        std::promise<void> firstEntered;
        std::promise<void> secondEntered;
        std::promise<void> firstRelease;
        std::promise<void> secondRelease;
        auto firstReleased = firstRelease.get_future().share();
        auto secondReleased = secondRelease.get_future().share();
        std::atomic<unsigned> detectorCalls{0};
        auto& desktop = test::desktopStubs();
        desktop.beforeDetection = [&] {
            if (detectorCalls.fetch_add(1) == 0) {
                firstEntered.set_value();
                firstReleased.wait();
            } else {
                secondEntered.set_value();
                secondReleased.wait();
            }
        };
        RegionSession session(fix.capture, fix.worker, fix.source);
        ReleaseScan releaseFirstOnExit{firstRelease};
        ReleaseScan releaseSecondOnExit{secondRelease};
        auto& picker = session.picker();
        picker.request(RegionPickerMode::AttachFace);
        (void)picker.openIfRequested(false);
        REQUIRE(firstEntered.get_future().wait_for(5s) == std::future_status::ready);
        picker.cancel();
        REQUIRE_FALSE(test::regionOverlayStubs().pickActive);
        picker.request(RegionPickerMode::AttachFace);
        (void)picker.openIfRequested(false);
        REQUIRE(secondEntered.get_future().wait_for(5s) == std::future_status::ready);
        releaseSecondOnExit.open();
        auto& delivered = test::regionOverlayStubs().deliveredFaces;
        REQUIRE(waitUntil([&] {
            picker.drainFaceScans();
            return delivered.contains(Display + 1);
        }));
        // A completed empty scan still marks its display scanned. Clearing the
        // delivery record makes any later stale update observable.
        delivered.clear();
        releaseFirstOnExit.open();
        REQUIRE(waitUntil([&] { return !picker.scansRunning(); }));
        picker.drainFaceScans();
        REQUIRE(delivered.empty());
        REQUIRE(picker.active());
        REQUIRE(detectorCalls.load() == 2);
        session.shutdown();
        REQUIRE_FALSE(session.backgroundWorkRunning());
        REQUIRE_FALSE(test::regionOverlayStubs().pickActive);
        desktop.beforeDetection = {};
    }
    std::fprintf(stderr, "stale_scan_reopen cycles=%u\n", cycles);
}

}  // namespace sidescopes
