#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include "app/face_tracking_worker.h"
#include "test_frame.h"

using namespace sidescopes;
using face_tracking::Action;
using face_tracking::Reason;
using Mode = FrameRegionResolution::Mode;

namespace {

void checkTrackedCrop(const FaceTrackingUpdate& update, LockRect detectedCrop, double faceWidth)
{
    const auto close = [faceWidth](double edge) {
        return Catch::Approx(edge).epsilon(0).margin(faceWidth * 0.02 + 1e-9);
    };
    CHECK(update.decision.crop.left == close(detectedCrop.left));
    CHECK(update.decision.crop.top == close(detectedCrop.top));
    CHECK(update.decision.crop.right == close(detectedCrop.right));
    CHECK(update.decision.crop.bottom == close(detectedCrop.bottom));
}

struct DetectorState
{
    int calls = 0;
    int created = 0;
    int destroyed = 0;
    std::thread::id owner = std::this_thread::get_id();
    FaceDetectionResult result{FaceDetectionStatus::Completed, {{40, 38, 20, 20}}};
    std::function<void(const FrameView&)> inspect;
};

struct Detector : FaceDetectionSession
{
    DetectorState& state;

    explicit Detector(DetectorState& value)
        : state(value)
    {
        ++state.created;
    }

    ~Detector() override
    {
        ++state.destroyed;
    }

    FaceDetectionResult detect(const FrameView& frame, double minimum) override
    {
        CHECK(std::this_thread::get_id() == state.owner);
        CHECK(minimum == 12.0);
        ++state.calls;
        if (state.inspect) {
            state.inspect(frame);
        }
        return state.result;
    }
};

struct Fixture
{
    DetectorState detector;
    double now = 10.0;
    int notified = 0;
    std::function<void()> onNotify;
    std::shared_ptr<FaceTrackingExchange> exchange = std::make_shared<FaceTrackingExchange>([this] {
        ++notified;
        if (onNotify) {
            onNotify();
        }
    });
    FaceTrackingWorker worker{exchange, [this] { return std::make_unique<Detector>(detector); },
                              [this] { return now; }};
    FaceTrackingCommand command;
    std::vector<uint8_t> pixels = std::vector<uint8_t>(std::size_t{528} * 96);
    FrameView frame{pixels.data(), 528, 128, 96};
    uint64_t seen = 0;

    Fixture()
    {
        command.revision = 1;
        command.lockGeneration = 1;
        command.identity = 42;
        command.captureEpoch = 7;
        command.displayId = 3;
        command.displayWidth = 128;
        command.displayHeight = 96;
        command.window = {0, 0, 128, 96};
        command.crop = face_lock::makeLock({64, 48, 20}, {59, 43, 69, 53});
        command.minimumFacePixels = 12;
        command.enabled = true;
        frame.sequence = 1;
        frame.stamp = {7, 3, now};
        exchange->select(command);
    }

    FrameRegionResolution run(bool fresh = true)
    {
        return worker.resolve({frame, RegionOfInterest{1, 2, 3, 4}, command.revision, fresh});
    }

    FaceTrackingUpdate update()
    {
        const auto value = exchange->fetch(seen);
        REQUIRE(value);
        return *value;
    }

    void next(double seconds)
    {
        now = seconds;
        ++frame.sequence;
        frame.stamp.receivedSeconds = seconds;
    }
};

struct SearchFixture : Fixture
{
    IntRect face{300, 220, 40, 40};
    std::vector<IntRect> searches;
    std::function<IntRect(IntRect)> response;

    SearchFixture()
    {
        pixels.resize(std::size_t{2576} * 480);
        frame = {pixels.data(), 2576, 640, 480};
        frame.sequence = 1;
        frame.stamp = {7, 3, now};
        command.displayWidth = 640;
        command.displayHeight = 480;
        command.window = {20, 20, 600, 440};
        command.crop = face_lock::makeLock({320, 240, 40}, {310, 230, 330, 250});
        exchange->select(command);
        detector.inspect = [this](const FrameView& crop) {
            const IntRect roi{crop.sourceX, crop.sourceY, crop.width, crop.height};
            searches.push_back(roi);
            const IntRect box = response ? response(roi) : face;
            detector.result.faces = {{box.x - roi.x, box.y - roi.y, box.width, box.height}};
        };
    }

    FaceTrackingUpdate follow(double seconds)
    {
        next(seconds);
        REQUIRE(run().mode == Mode::Override);
        const auto result = update();
        REQUIRE(result.decision.action == Action::Accepted);
        command.crop.lastAnchor = result.decision.anchor;
        exchange->select(command);
        return result;
    }
};

}  // namespace

TEST_CASE("Analysis allocation recovery preserves ambiguous tracking and consumes failed pixels")
{
    Fixture fix;
    FrameMailbox mailbox;
    AnalysisWorker analysis(mailbox);
    AnalysisSettings settings;
    settings.region = RegionOfInterest{};
    settings.selectionRevision = fix.command.revision;
    settings.enabledScopes = {"org.sidescopes.vectorscope"};
    analysis.updateSettings(settings);
    int factories = 0;
    analysis.setFrameRegionResolverFactory([&] {
        ++factories;
        auto tracker = std::make_shared<FaceTrackingWorker>(
            fix.exchange, [&] { return std::make_unique<Detector>(fix.detector); }, [&] { return fix.now; });
        return [tracker](const FrameRegionRequest& request) { return tracker->resolve(request); };
    });
    analysis.startInline();
    const auto publish = [&] {
        auto frame = test::makeSolidFrameBuffer(128, 96, Color{191, 0, 0}, fix.frame.sequence);
        frame.stamp = fix.frame.stamp;
        mailbox.publish(std::move(frame));
        analysis.pump();
    };
    fix.detector.result.faces.push_back({46, 38, 20, 20});
    publish();
    REQUIRE(fix.update().decision.reason == Reason::Ambiguous);

    fix.next(10.04);
    fix.detector.inspect = [](const FrameView&) { throw std::bad_alloc(); };
    REQUIRE_NOTHROW(publish());
    CHECK(analysis.consumedFrameSequence() == 2);
    CHECK(fix.detector.calls == 2);
    CHECK_FALSE(fix.exchange->fetch(fix.seen));
    analysis.updateSettings(settings);
    analysis.pump();
    CHECK(fix.detector.calls == 2);
    CHECK(factories == 1);

    fix.detector.inspect = {};
    fix.detector.result.faces.resize(1);
    fix.next(10.08);
    publish();
    const auto held = fix.update();
    CHECK(held.decision.action == Action::Held);
    CHECK(held.decision.reason == Reason::Ambiguous);
    CHECK(factories == 1);
    fix.next(11.01);
    publish();
    CHECK(fix.update().decision.action == Action::Stopped);
    const int calls = fix.detector.calls;
    fix.next(11.05);
    publish();
    CHECK(fix.update().decision.action == Action::Stopped);
    CHECK(fix.detector.calls == calls);
}

TEST_CASE("Tracking detects the current frame ROI with its native format and padded stride")
{
    Fixture fix;
    SECTION("BGRA")
    {
        fix.frame.format = PixelFormat::Bgra8;
    }
    SECTION("10-bit")
    {
        fix.frame.format = PixelFormat::Argb2101010;
    }
    for (std::size_t i = 0; i < fix.pixels.size(); ++i) {
        fix.pixels[i] = static_cast<uint8_t>(i % 251);
    }
    fix.detector.inspect = [&](const FrameView& crop) {
        CHECK(crop.pixels == fix.frame.rawPixelAt(14, 0));
        CHECK(crop.rawPixelAt(99, 95) == fix.frame.rawPixelAt(113, 95));
        CHECK(crop.strideBytes == 528);
        CHECK(crop.width == 100);
        CHECK(crop.height == 96);
        CHECK(crop.sourceX == 14);
        CHECK(crop.sourceY == 0);
        CHECK(crop.sourceWidth == 128);
        CHECK(crop.sourceHeight == 96);
        CHECK(crop.format == fix.frame.format);
        CHECK(crop.sequence == fix.frame.sequence);
        CHECK(crop.sampleAt(7, 8).r == fix.frame.sampleAt(21, 8).r);
    };
    const auto result = fix.run();
    REQUIRE(result.mode == Mode::Override);
    REQUIRE(result.region);
    CHECK(result.region->leftPercent == Catch::Approx(59.0 / 128 * 100));
    CHECK(fix.update().decision.action == Action::Accepted);
    fix.next(10.04);
    CHECK(fix.run().mode == Mode::Override);
    CHECK(fix.detector.calls == 2);
    CHECK(fix.detector.created == 1);
}

TEST_CASE("Settings-only resolution retains clean still-photo tracking without detecting again")
{
    Fixture fix;
    REQUIRE(fix.run().mode == Mode::Override);
    (void)fix.update();
    fix.now = 60.0;
    CHECK(fix.run(false).mode == Mode::Override);
    const auto update = fix.update();
    CHECK(update.decision.following);
    CHECK(update.decision.reason == Reason::Waiting);
    CHECK(fix.detector.calls == 1);
    fix.next(60.01);
    CHECK(fix.run().mode == Mode::Override);
    CHECK(fix.update().decision.action == Action::Accepted);
}

TEST_CASE("Face stabilization publishes the same crop to scopes and the border")
{
    SearchFixture fix;
    const auto first = fix.follow(10.04);
    ++fix.face.x;
    const auto moved = fix.follow(10.08);
    CHECK(moved.decision.anchor.centerX > first.decision.anchor.centerX);
    CHECK(moved.decision.anchor.centerX < 321.0);
    const auto mapped = face_lock::mapRegion(fix.command.crop, moved.decision.anchor);
    CHECK(moved.region.leftPercent == Catch::Approx(mapped.left / 640.0 * 100));
    CHECK(moved.region.rightPercent == Catch::Approx(mapped.right / 640.0 * 100));
    fix.now = 20.0;
    const auto resolution = fix.run(false);
    REQUIRE(resolution.region);
    CHECK(*resolution.region == moved.region);
    const auto held = fix.update();
    CHECK(held.region == moved.region);
    CHECK(held.decision.action == Action::Held);
    CHECK(held.decision.reason == Reason::Waiting);
    CHECK(held.decision.evidenceSourceSeconds == moved.decision.evidenceSourceSeconds);
    CHECK(fix.detector.calls == 2);
}

TEST_CASE("A detector search follows raw evidence rather than the stabilized border")
{
    SearchFixture fix;
    (void)fix.follow(10.04);
    fix.face.x += 20;
    (void)fix.follow(10.14);
    fix.face.x += 2;
    const auto smoothed = fix.follow(10.18);
    REQUIRE(smoothed.decision.anchor.centerX < 342.0);
    (void)fix.follow(10.22);
    // The raw anchor is342; recentering from the filtered value would start
    // one pixel earlier and change the detector's input sampling grid.
    CHECK(fix.searches.back().x == 242);
}

TEST_CASE("Lost-face resolution keeps the last stabilized crop and recovery discards old easing")
{
    SearchFixture fix;
    (void)fix.follow(10.04);
    ++fix.face.x;
    const auto moved = fix.follow(10.08);
    fix.response = [](IntRect) { return IntRect{}; };
    fix.next(10.12);
    REQUIRE(fix.run().mode == Mode::Override);
    const auto held = fix.update();
    CHECK(held.decision.action == Action::Held);
    CHECK(held.region == moved.region);
    SECTION("stopped")
    {
        fix.now = 11.13;
        const auto resolution = fix.run(false);
        REQUIRE(resolution.region);
        CHECK(*resolution.region == moved.region);
        const auto retired = fix.update();
        CHECK(retired.decision.action == Action::Stopped);
        CHECK(retired.region == moved.region);
    }
    SECTION("brief recovery")
    {
        fix.response = {};
        ++fix.face.x;
        const auto recovered = fix.follow(10.16);
        CHECK(recovered.decision.anchor.centerX == 322.0);
    }
}

TEST_CASE("In-flight command changes cannot publish stale tracking updates")
{
    Fixture fix;
    fix.detector.inspect = [&](const FrameView&) {
        auto replacement = fix.command;
        SECTION("Revision")
        {
            ++replacement.revision;
        }
        SECTION("Epoch")
        {
            ++replacement.captureEpoch;
        }
        SECTION("Display")
        {
            ++replacement.displayId;
        }
        SECTION("Window")
        {
            ++replacement.identity;
        }
        SECTION("Disabled")
        {
            replacement.enabled = false;
        }
        fix.exchange->select(replacement);
    };
    CHECK(fix.run().mode == Mode::Skip);
    CHECK_FALSE(fix.exchange->fetch(fix.seen));
    CHECK(fix.notified == 0);
}

TEST_CASE("Native failure stops tracking after grace without more detector calls")
{
    Fixture fix;
    SECTION("Failed status")
    {
        fix.detector.result = {FaceDetectionStatus::Failed, {}};
    }
    SECTION("Thrown native error")
    {
        fix.detector.inspect = [](const FrameView&) { throw std::runtime_error("native failure"); };
    }
    REQUIRE(fix.run().mode == Mode::Override);
    CHECK(fix.update().decision.reason == Reason::NativeFailure);
    fix.now = 10.2;
    CHECK(fix.run(false).mode == Mode::Override);
    CHECK(fix.update().decision.reason == Reason::NativeFailure);
    fix.now = 11.01;
    CHECK(fix.run(false).mode == Mode::Suppress);
    CHECK(fix.update().decision.action == Action::Stopped);
    fix.detector.inspect = {};
    fix.next(11.05);
    CHECK(fix.run().mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    CHECK(fix.detector.calls == 1);
}

TEST_CASE("Unsupported detection retires the lock immediately")
{
    Fixture fix;
    fix.detector.result = {FaceDetectionStatus::Unsupported, {}};
    REQUIRE(fix.run().mode == Mode::Suppress);
    CHECK(fix.update().decision.reason == Reason::Unsupported);
    fix.next(10.05);
    CHECK(fix.run().mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    CHECK(fix.detector.calls == 1);
}

TEST_CASE("The tracking deadline stops fresh frames before native work")
{
    Fixture fix;
    fix.detector.result.faces.clear();
    REQUIRE(fix.run().mode == Mode::Override);
    const auto missing = fix.update();
    REQUIRE(missing.decision.uncertaintyDeadline == 11.0);
    fix.detector.result.faces = {{40, 38, 20, 20}};
    fix.next(10.999);
    REQUIRE(fix.run().mode == Mode::Override);
    CHECK(fix.update().decision.action == Action::Held);
    REQUIRE(fix.detector.calls == 2);
    fix.next(11.0);
    REQUIRE(fix.run().mode == Mode::Suppress);
    const auto stopped = fix.update();
    CHECK(stopped.decision.action == Action::Stopped);
    CHECK_FALSE(stopped.decision.following);
    CHECK(stopped.decision.uncertaintyDeadline == missing.decision.uncertaintyDeadline);
    CHECK(fix.detector.calls == 2);
    fix.next(30.0);
    CHECK(fix.run().mode == Mode::Suppress);
    CHECK(fix.detector.calls == 2);
}

TEST_CASE("A native result completing at the tracking deadline cannot restore readings")
{
    Fixture fix;
    fix.detector.result.faces.clear();
    REQUIRE(fix.run().mode == Mode::Override);
    const auto missing = fix.update();
    fix.next(10.99);
    fix.detector.result.faces = {{40, 38, 20, 20}};
    fix.detector.inspect = [&](const FrameView&) { fix.now = 11.0; };
    SECTION("Completed face")
    {
    }
    SECTION("Unsupported result")
    {
        fix.detector.result = {FaceDetectionStatus::Unsupported, {}};
    }
    SECTION("Removed selection while native work is in flight")
    {
        fix.detector.inspect = [&](const FrameView&) {
            fix.now = 11.0;
            fix.command.enabled = false;
            ++fix.command.revision;
            fix.exchange->select(fix.command);
        };
        CHECK(fix.run().mode == Mode::Skip);
        CHECK_FALSE(fix.exchange->fetch(fix.seen));
        CHECK(fix.detector.calls == 2);
        return;
    }
    REQUIRE(fix.run().mode == Mode::Suppress);
    const auto stopped = fix.update();
    CHECK(stopped.decision.action == Action::Stopped);
    CHECK_FALSE(stopped.decision.following);
    CHECK_FALSE(stopped.decision.selectedBox);
    CHECK(stopped.decision.evidenceSourceSeconds == missing.decision.evidenceSourceSeconds);
    CHECK(stopped.decision.uncertaintyDeadline == missing.decision.uncertaintyDeadline);
    CHECK(fix.detector.calls == 2);
}

TEST_CASE("Invalid frames and commands are rejected before native pixel access")
{
    Fixture fix;
    SECTION("Null pixels")
    {
        fix.frame.pixels = nullptr;
    }
    SECTION("Short row")
    {
        fix.frame.strideBytes = 511;
    }
    SECTION("Negative stride")
    {
        fix.frame.strideBytes = -1;
    }
    SECTION("Invalid format")
    {
        fix.frame.format = static_cast<PixelFormat>(99);
    }
    SECTION("Cropped frame")
    {
        fix.frame.sourceX = 1;
    }
    SECTION("Foreign epoch")
    {
        ++fix.frame.stamp.captureEpoch;
    }
    SECTION("Foreign display")
    {
        ++fix.frame.stamp.displayId;
    }
    SECTION("Invalid time")
    {
        fix.frame.stamp.receivedSeconds = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("Future time")
    {
        fix.frame.stamp.receivedSeconds = 11;
    }
    SECTION("Invalid anchor")
    {
        fix.command.crop.lastAnchor.centerX = std::numeric_limits<double>::infinity();
    }
    SECTION("Invalid crop")
    {
        fix.command.crop.sizeX = -1;
    }
    SECTION("Invalid floor")
    {
        fix.command.minimumFacePixels = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("Overflow window")
    {
        fix.command.window.x = std::numeric_limits<int>::max();
    }
    fix.exchange->select(fix.command);
    CHECK_NOTHROW(fix.run());
    CHECK(fix.run().mode == Mode::Skip);
    CHECK(fix.detector.calls == 0);
}

TEST_CASE("Malformed native geometry is not silently hidden beside a valid face")
{
    Fixture fix;
    SECTION("Negative origin")
    {
        fix.detector.result.faces.push_back({-1, 10, 20, 20});
    }
    SECTION("Zero width")
    {
        fix.detector.result.faces.push_back({10, 10, 0, 20});
    }
    SECTION("Overflow extent")
    {
        fix.detector.result.faces.push_back({1, 1, std::numeric_limits<int>::max(), 20});
    }
    SECTION("Outside ROI")
    {
        fix.detector.result.faces.push_back({90, 10, 20, 20});
    }
    REQUIRE(fix.run().mode == Mode::Override);
    CHECK(fix.update().decision.reason == Reason::InvalidDetection);
}

TEST_CASE("Valid side-clipped detections are intentionally excluded")
{
    Fixture fix;
    fix.detector.result.faces = {{0, 20, 20, 20}};
    REQUIRE(fix.run().mode == Mode::Override);
    CHECK(fix.update().decision.reason == Reason::SuccessfulEmpty);
}

TEST_CASE("Expired native work cannot move the crop")
{
    Fixture fix;
    fix.detector.result.faces = {{44, 38, 20, 20}};
    fix.detector.inspect = [&](const FrameView&) { fix.now += 0.21; };
    REQUIRE(fix.run().mode == Mode::Override);
    const auto update = fix.update();
    CHECK(update.decision.reason == Reason::ExpiredResult);
    CHECK(update.decision.crop.left == 59.0);
    CHECK_FALSE(update.decision.selectedBox);
}

TEST_CASE("Exchange notification runs outside its lock and can invalidate the command")
{
    FaceTrackingCommand command;
    command.revision = 1;
    command.identity = 42;
    command.captureEpoch = 7;
    command.displayId = 3;
    command.enabled = true;
    std::shared_ptr<FaceTrackingExchange> exchange;
    exchange = std::make_shared<FaceTrackingExchange>([&] {
        CHECK(exchange->selection().identity == 42);
        command.enabled = false;
        exchange->select(command);
    });
    exchange->select(command);
    FaceTrackingUpdate update;
    update.revision = 1;
    update.identity = 42;
    update.stamp = {7, 3, 10.0};
    update.decision.action = Action::Held;
    CHECK(exchange->publish(update));
    uint64_t seen = 0;
    CHECK_FALSE(exchange->fetch(seen));
}

TEST_CASE("Crop revisions and temporary disabling cannot restart stopped tracking")
{
    Fixture fix;
    fix.detector.result.faces.push_back({46, 38, 20, 20});
    REQUIRE(fix.run().mode == Mode::Override);
    REQUIRE(fix.update().decision.reason == Reason::Ambiguous);
    fix.command.enabled = false;
    ++fix.command.revision;
    fix.exchange->select(fix.command);
    CHECK(fix.run(false).mode == Mode::Configured);
    fix.command.enabled = true;
    ++fix.command.revision;
    fix.command.crop.sizeX = 0.25;
    fix.exchange->select(fix.command);
    fix.now = 10.1;
    CHECK(fix.run(false).mode == Mode::Override);
    CHECK(fix.update().decision.reason == Reason::Ambiguous);
    fix.now = 11.01;
    CHECK(fix.run(false).mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    ++fix.command.revision;
    fix.exchange->select(fix.command);
    fix.next(11.05);
    CHECK(fix.run().mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    CHECK(fix.detector.calls == 1);
    ++fix.command.lockGeneration;
    ++fix.command.revision;
    fix.exchange->select(fix.command);
    fix.detector.result.faces.resize(1);
    fix.next(11.1);
    CHECK(fix.run().mode == Mode::Override);
    CHECK(fix.update().decision.action == Action::Accepted);
}

TEST_CASE("Returning to another saved window does not reset its ambiguous identity")
{
    Fixture fix;
    fix.detector.result.faces.push_back({46, 38, 20, 20});
    REQUIRE(fix.run().mode == Mode::Override);
    REQUIRE(fix.update().decision.reason == Reason::Ambiguous);
    auto other = fix.command;
    other.identity = 99;
    other.lockGeneration = 2;
    other.revision = 2;
    fix.exchange->select(other);
    CHECK(fix.worker.resolve({fix.frame, {}, 2, false}).mode == Mode::Override);
    ++fix.command.revision;
    ++fix.command.revision;
    fix.exchange->select(fix.command);
    fix.now = 11.01;
    CHECK(fix.run(false).mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    CHECK(fix.detector.calls == 1);
}

TEST_CASE("Mechanical window translation moves the crop without observing new face pixels")
{
    Fixture fix;
    fix.command.window = {10, 0, 108, 96};
    fix.exchange->select(fix.command);
    REQUIRE(fix.run().mode == Mode::Override);
    const auto previous = fix.update();
    ++fix.command.revision;
    fix.command.window.x += 5;
    face_lock::translate(fix.command.crop, 5, 0);
    fix.exchange->select(fix.command);
    CHECK(fix.run(false).mode == Mode::Override);
    const auto moved = fix.update();
    CHECK(moved.decision.crop.left == previous.decision.crop.left + 5);
    CHECK(moved.decision.evidenceSourceSeconds == previous.decision.evidenceSourceSeconds);
    CHECK(moved.decision.action == Action::Held);
    CHECK(fix.detector.calls == 1);
}

TEST_CASE("Source replacement retires the same explicit lock and suppresses readings")
{
    Fixture fix;
    REQUIRE(fix.run().mode == Mode::Override);
    (void)fix.update();
    ++fix.command.revision;
    ++fix.command.captureEpoch;
    fix.frame.stamp.captureEpoch = fix.command.captureEpoch;
    fix.exchange->select(fix.command);
    fix.next(10.05);
    CHECK(fix.run().mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    CHECK(fix.detector.calls == 1);
}

TEST_CASE("A proven source resume accepts only the new physical frame epoch")
{
    Fixture fix;
    fix.command.captureContinuity = 7;
    fix.exchange->select(fix.command);
    REQUIRE(fix.run().mode == Mode::Override);
    const auto original = fix.update();
    ++fix.command.revision;
    ++fix.command.captureEpoch;
    fix.exchange->select(fix.command);
    fix.next(10.05);
    CHECK(fix.run().mode == Mode::Skip);  // a late old-stream frame
    CHECK(fix.detector.calls == 1);
    fix.frame.stamp.captureEpoch = fix.command.captureEpoch;
    fix.frame.sequence = 1;  // native streams may restart their numbering
    REQUIRE(fix.run().mode == Mode::Override);
    const auto resumed = fix.update();
    CHECK(resumed.decision.following);
    CHECK(resumed.decision.action == Action::Accepted);
    CHECK(resumed.decision.crop.left == original.decision.crop.left);
    CHECK(resumed.decision.crop.top == original.decision.crop.top);
    CHECK(resumed.decision.crop.right == original.decision.crop.right);
    CHECK(resumed.decision.crop.bottom == original.decision.crop.bottom);
    CHECK(fix.detector.calls == 2);
}

TEST_CASE("Stream numbering resets never reset the monotonic receipt-time guard")
{
    Fixture fix;
    fix.command.captureContinuity = 7;
    fix.exchange->select(fix.command);
    REQUIRE(fix.run().mode == Mode::Override);
    (void)fix.update();
    ++fix.command.revision;
    ++fix.command.captureEpoch;
    fix.exchange->select(fix.command);
    fix.frame.stamp.captureEpoch = fix.command.captureEpoch;
    fix.frame.sequence = 1;
    fix.now = 10.05;
    SECTION("Equal receipt time in the new physical stream")
    {
        fix.frame.stamp.receivedSeconds = 10.0;
    }
    SECTION("Receipt time moves backwards in the new physical stream")
    {
        fix.frame.stamp.receivedSeconds = 9.99;
    }
    CHECK(fix.run().mode == Mode::Skip);
    CHECK_FALSE(fix.exchange->fetch(fix.seen));
    // Rejection preserves the prior and its finite motion state; the first
    // truly later current-stream observation still follows the selected face.
    fix.frame.sequence = 2;
    fix.frame.stamp.receivedSeconds = 10.05;
    REQUIRE(fix.run().mode == Mode::Override);
    const auto resumed = fix.update();
    CHECK(resumed.decision.action == Action::Accepted);
    CHECK(resumed.decision.following);
    CHECK(resumed.decision.evidenceSourceSeconds == 10.05);
}

TEST_CASE("Resume continuity never permits a foreign display or pixel grid")
{
    Fixture fix;
    fix.command.captureContinuity = 7;
    fix.exchange->select(fix.command);
    REQUIRE(fix.run().mode == Mode::Override);
    (void)fix.update();
    ++fix.command.revision;
    ++fix.command.captureEpoch;
    fix.frame.stamp.captureEpoch = fix.command.captureEpoch;
    SECTION("Foreign display")
    {
        ++fix.command.displayId;
        fix.frame.stamp.displayId = fix.command.displayId;
    }
    SECTION("New display-pixel grid")
    {
        // A narrower full frame has valid storage and bounds but a different grid.
        --fix.command.displayWidth;
        --fix.command.window.width;
        --fix.frame.width;
    }
    fix.exchange->select(fix.command);
    fix.next(10.05);
    REQUIRE(fix.run().mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    CHECK(fix.detector.calls == 1);
}

TEST_CASE("Source resume cannot restart stopped tracking")
{
    Fixture fix;
    fix.command.captureContinuity = 7;
    fix.exchange->select(fix.command);
    REQUIRE(fix.run().mode == Mode::Override);
    (void)fix.update();
    fix.detector.result.faces.clear();
    fix.next(10.05);
    REQUIRE(fix.run().mode == Mode::Override);
    (void)fix.update();
    fix.now = 11.06;
    REQUIRE(fix.run(false).mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    const int calls = fix.detector.calls;
    ++fix.command.revision;
    ++fix.command.captureEpoch;
    fix.frame.stamp.captureEpoch = fix.command.captureEpoch;
    fix.exchange->select(fix.command);
    fix.next(11.10);
    REQUIRE(fix.run().mode == Mode::Suppress);
    CHECK_FALSE(fix.update().decision.following);
    CHECK(fix.detector.calls == calls);
}

TEST_CASE("Removed locks cannot be revived by an enabled stale command")
{
    Fixture fix;
    REQUIRE(fix.run().mode == Mode::Override);
    (void)fix.update();
    fix.command.activeLocks = std::make_shared<const std::map<uint64_t, uint64_t>>();
    fix.exchange->select(fix.command);
    CHECK(fix.run().mode == Mode::Skip);
    CHECK(fix.detector.calls == 1);
    CHECK_FALSE(fix.exchange->fetch(fix.seen));
}

TEST_CASE("A native allocation failure remains visible to the analysis allocation boundary")
{
    Fixture fix;
    fix.detector.inspect = [](const FrameView&) { throw std::bad_alloc(); };
    CHECK_THROWS_AS(fix.run(), std::bad_alloc);
    CHECK_FALSE(fix.exchange->fetch(fix.seen));
}

TEST_CASE("Discarded in-flight work does not alter the next revision's anchor")
{
    Fixture fix;
    fix.detector.result.faces = {{44, 38, 20, 20}};
    fix.detector.inspect = [&](const FrameView&) {
        ++fix.command.revision;
        fix.exchange->select(fix.command);
    };
    CHECK(fix.run().mode == Mode::Skip);
    fix.detector.inspect = {};
    fix.detector.result.faces = {{40, 38, 20, 20}};
    fix.next(10.04);
    REQUIRE(fix.run().mode == Mode::Override);
    const auto accepted = fix.update();
    CHECK(accepted.decision.action == Action::Accepted);
    CHECK(accepted.decision.crop.left == 59);
}

TEST_CASE("Automatic anchor feedback does not turn a video frame into a control revision")
{
    Fixture fix;
    fix.detector.result.faces = {{44, 38, 20, 20}};
    REQUIRE(fix.run().mode == Mode::Override);
    const auto accepted = fix.update();
    fix.command.crop.lastAnchor = accepted.decision.anchor;
    fix.exchange->select(fix.command);
    fix.next(10.04);
    REQUIRE(fix.run().mode == Mode::Override);
    CHECK(fix.update().decision.action == Action::Accepted);
    CHECK(fix.detector.calls == 2);
}

TEST_CASE("Missing and failing detector factories are explicit native outcomes")
{
    Fixture fix;
    FaceSessionFactory factory;
    Reason expected = Reason::Unsupported;
    SECTION("Missing factory")
    {
    }
    SECTION("Throwing factory")
    {
        factory = []() -> std::unique_ptr<FaceDetectionSession> { throw std::runtime_error("model unavailable"); };
        expected = Reason::NativeFailure;
    }
    FaceTrackingWorker worker{fix.exchange, factory, [&] { return fix.now; }};
    const auto mode = expected == Reason::Unsupported ? Mode::Suppress : Mode::Override;
    REQUIRE(worker.resolve({fix.frame, {}, fix.command.revision, true}).mode == mode);
    CHECK(fix.update().decision.reason == expected);
}

TEST_CASE("Identical frames keep a stable detector grid despite grid-dependent face boxes")
{
    SearchFixture fix;
    fix.response = [](IntRect roi) {
        // Model an input-grid-sensitive detector on the same full image. A
        // search rebuilt from either raw box would select the other branch.
        return roi.x == 220 && roi.y == 140 ? IntRect{302, 221, 40, 40} : IntRect{298, 219, 40, 40};
    };
    const auto first = fix.follow(10.0);
    for (int i = 1; i < 12; ++i) {
        const auto current = fix.follow(10.0 + i * 0.04);
        CHECK(fix.searches.back() == fix.searches.front());
        CHECK(current.decision.crop.left == first.decision.crop.left);
        CHECK(current.decision.crop.top == first.decision.crop.top);
        CHECK(current.decision.crop.right == first.decision.crop.right);
        CHECK(current.decision.crop.bottom == first.decision.crop.bottom);
        CHECK(current.frameSequence == fix.frame.sequence);
    }
    CHECK(fix.detector.calls == 12);
}

TEST_CASE("A fixed detector search still applies current movement and scale on the same frame")
{
    SearchFixture fix;
    const auto original = fix.follow(10.0);
    fix.face = {305, 223, 40, 40};
    const auto moved = fix.follow(10.04);
    CHECK(moved.decision.crop.left > original.decision.crop.left);
    CHECK(moved.decision.crop.top > original.decision.crop.top);
    checkTrackedCrop(moved, {315, 233, 335, 253}, 40);
    CHECK(moved.frameSequence == fix.frame.sequence);
    fix.face = {303, 221, 44, 44};
    const auto scaled = fix.follow(10.08);
    checkTrackedCrop(scaled, {314, 232, 336, 254}, 44);
    CHECK(fix.searches[1] == fix.searches[0]);
    CHECK(fix.searches[2] == fix.searches[0]);
}

TEST_CASE("Approaching the detector search margin recenters coverage without delaying output")
{
    SearchFixture fix;
    (void)fix.follow(10.0);
    for (int i = 1; i <= 4; ++i) {
        fix.face.x = 300 + i * 8;
        const auto moved = fix.follow(10.0 + i * 0.04);
        checkTrackedCrop(moved, {310.0 + i * 8, 230, 330.0 + i * 8, 250}, 40);
        if (i <= 3) {
            CHECK(fix.searches.back() == fix.searches.front());
        }
    }
    CHECK(fix.searches.back() == IntRect{244, 140, 200, 200});
    const auto recentered = fix.searches.back();
    (void)fix.follow(10.20);
    CHECK(fix.searches.back() == recentered);
}

TEST_CASE("Detector search scale hysteresis refreshes both growth and shrinking")
{
    SearchFixture fix;
    (void)fix.follow(10.0);
    std::vector<int> widths;
    int expectedExtent = 0;
    SECTION("Growing face")
    {
        widths = {50, 60};
        expectedExtent = 300;
    }
    SECTION("Shrinking face")
    {
        widths = {34, 28, 24};
        expectedExtent = 120;
    }
    for (std::size_t i = 0; i < widths.size(); ++i) {
        const int width = widths[i];
        fix.face = {320 - width / 2, 240 - width / 2, width, width};
        const auto scaled = fix.follow(10.04 + static_cast<double>(i) * 0.04);
        const double half = width * 0.25;
        checkTrackedCrop(scaled, {320 - half, 240 - half, 320 + half, 240 + half}, width);
        CHECK(fix.searches.back() == fix.searches.front());
    }
    (void)fix.follow(10.16);
    CHECK(fix.searches.back().width == expectedExtent);
    CHECK(fix.searches.back().height == expectedExtent);
    const auto resized = fix.searches.back();
    (void)fix.follow(10.20);
    CHECK(fix.searches.back() == resized);
}

TEST_CASE("Parent translation carries the detector grid and parent resizing invalidates it")
{
    SearchFixture fix;
    (void)fix.follow(10.0);
    fix.face.x += 10;
    fix.face.y += 5;
    fix.command.window.x += 10;
    fix.command.window.y += 5;
    face_lock::translate(fix.command.crop, 10, 5);
    ++fix.command.revision;
    fix.exchange->select(fix.command);
    CHECK(fix.run(false).mode == Mode::Override);
    (void)fix.update();
    CHECK(fix.detector.calls == 1);
    const auto moved = fix.follow(10.04);
    CHECK(moved.decision.crop.left == 320);
    CHECK(moved.decision.crop.top == 235);
    CHECK(fix.searches.back() == IntRect{230, 145, 200, 200});
    // A small accepted motion must not recenter the translated search.
    fix.face.x += 3;
    (void)fix.follow(10.08);
    CHECK(fix.searches.back() == IntRect{230, 145, 200, 200});
    --fix.command.window.width;
    ++fix.command.revision;
    fix.exchange->select(fix.command);
    (void)fix.follow(10.12);
    // Resizing resets from the saved, stabilized anchor at332.2. Enclose its
    // fractional search edges outward; later raw evidence drives reuse again.
    CHECK(fix.searches.back() == IntRect{232, 145, 201, 200});
}

TEST_CASE("Crop edits and proven source resumes retain a safe detector grid")
{
    SearchFixture fix;
    fix.command.captureContinuity = 7;
    fix.exchange->select(fix.command);
    fix.face.x += 3;
    (void)fix.follow(10.0);
    ++fix.command.revision;
    SECTION("Manual crop edit")
    {
        fix.command.crop.sizeX = 0.25;
    }
    SECTION("Healthy source resume")
    {
        ++fix.command.captureEpoch;
        fix.frame.stamp.captureEpoch = fix.command.captureEpoch;
        fix.frame.sequence = 0;
    }
    fix.exchange->select(fix.command);
    (void)fix.follow(10.04);
    CHECK(fix.searches.back() == fix.searches.front());
}

TEST_CASE("A saved crop resumes without rebinding around an unconsumed animation anchor")
{
    SearchFixture fix;
    fix.command.captureContinuity = 7;
    fix.exchange->select(fix.command);
    const auto original = fix.follow(10.0);
    const auto saved = fix.command.crop;
    const auto originalFace = fix.face;
    const auto originalSearch = fix.searches.front();

    // Inspect the publication but do not feed its anchor into the saved UI
    // command: minimization can begin between a fresh frame and UI consumption.
    fix.face = {305, 222, 38, 38};
    fix.next(10.04);
    REQUIRE(fix.run().mode == Mode::Override);
    const auto animation = fix.update();
    REQUIRE(animation.decision.action == Action::Accepted);
    REQUIRE(animation.decision.anchor.centerX != saved.lastAnchor.centerX);
    REQUIRE(animation.decision.anchor.width != saved.lastAnchor.width);
    REQUIRE(fix.searches.back() == originalSearch);

    fix.command.enabled = false;
    ++fix.command.revision;
    fix.exchange->select(fix.command);
    REQUIRE(fix.run(false).mode == Mode::Configured);
    fix.command.enabled = true;
    ++fix.command.revision;
    ++fix.command.captureEpoch;
    fix.exchange->select(fix.command);
    fix.next(10.08);
    REQUIRE(fix.run().mode == Mode::Skip);  // old stream cannot restore the crop
    fix.frame.stamp.captureEpoch = fix.command.captureEpoch;
    fix.frame.sequence = 1;
    REQUIRE(fix.run(false).mode == Mode::Override);
    const auto restored = fix.update();
    CHECK(restored.region == original.region);
    CHECK(restored.decision.anchor.centerX == saved.lastAnchor.centerX);
    CHECK(restored.decision.anchor.centerY == saved.lastAnchor.centerY);
    CHECK(restored.decision.anchor.width == saved.lastAnchor.width);
    CHECK(restored.decision.evidenceSourceSeconds == animation.decision.evidenceSourceSeconds);
    CHECK(fix.detector.calls == 2);

    fix.face = originalFace;
    const auto fresh = fix.follow(10.12);
    CHECK(fresh.region == original.region);
    CHECK(fresh.decision.crop.left == 310);
    CHECK(fresh.decision.crop.top == 230);
    CHECK(fresh.decision.crop.right == 330);
    CHECK(fresh.decision.crop.bottom == 250);
    CHECK(fix.searches.back() == originalSearch);
    fix.face.x += 3;
    const auto moved = fix.follow(10.16);
    checkTrackedCrop(moved, {313, 230, 333, 250}, 40);
    CHECK(fix.searches.back() == originalSearch);
}

TEST_CASE("Stale control remaps cannot commit a different detector grid")
{
    SearchFixture fix;
    fix.face.x += 3;
    (void)fix.follow(10.0);
    const auto original = fix.searches.front();
    const auto inspect = fix.detector.inspect;
    const int originalWidth = fix.command.window.width;
    --fix.command.window.width;
    ++fix.command.revision;
    fix.exchange->select(fix.command);
    const auto replace = [&] {
        fix.command.window.width = originalWidth;
        ++fix.command.revision;
        fix.exchange->select(fix.command);
    };
    SECTION("Selection changes during native detection")
    {
        fix.detector.inspect = [&](const FrameView& crop) {
            inspect(crop);
            replace();
        };
    }
    SECTION("Selection changes during publication notification")
    {
        fix.onNotify = replace;
    }
    fix.next(10.04);
    CHECK(fix.run().mode == Mode::Skip);
    CHECK_FALSE(fix.exchange->fetch(fix.seen));
    CHECK(fix.searches.back().x == original.x + 3);
    fix.detector.inspect = inspect;
    fix.onNotify = {};
    (void)fix.follow(10.08);
    CHECK(fix.searches.back() == original);
}

TEST_CASE("A parent edge does not force detector search changes for small face movement")
{
    SearchFixture fix;
    fix.command.crop = face_lock::makeLock({64, 240, 40}, {54, 230, 74, 250});
    fix.face = {44, 220, 40, 40};
    fix.exchange->select(fix.command);
    const auto first = fix.follow(10.0);
    CHECK(fix.searches.front() == IntRect{20, 140, 144, 200});
    fix.face.x += 2;
    const auto moved = fix.follow(10.04);
    CHECK(moved.decision.crop.left > first.decision.crop.left);
    CHECK(moved.decision.crop.left <= first.decision.crop.left + 2);
    (void)fix.follow(10.08);
    CHECK(fix.searches[1] == fix.searches[0]);
    CHECK(fix.searches[2] == fix.searches[0]);
}

TEST_CASE("A new live lock generation cannot inherit the previous detector search")
{
    SearchFixture fix;
    fix.command.activeLocks = std::make_shared<const std::map<uint64_t, uint64_t>>(
        std::map<uint64_t, uint64_t>{{fix.command.identity, fix.command.lockGeneration}});
    fix.exchange->select(fix.command);
    fix.face.x += 3;
    (void)fix.follow(10.0);
    REQUIRE(fix.searches.front().x == 220);
    SECTION("Direct generation replacement")
    {
    }
    SECTION("The previous lock was removed before the new selection")
    {
        fix.command.enabled = false;
        fix.command.activeLocks = std::make_shared<const std::map<uint64_t, uint64_t>>();
        ++fix.command.revision;
        fix.exchange->select(fix.command);
        CHECK(fix.run(false).mode == Mode::Configured);
    }
    fix.command.enabled = true;
    ++fix.command.lockGeneration;
    ++fix.command.revision;
    fix.command.activeLocks = std::make_shared<const std::map<uint64_t, uint64_t>>(
        std::map<uint64_t, uint64_t>{{fix.command.identity, fix.command.lockGeneration}});
    fix.exchange->select(fix.command);
    const auto selected = fix.follow(10.04);
    CHECK(selected.decision.crop.left == 313);
    CHECK(fix.searches.back() == IntRect{223, 140, 200, 200});
}

TEST_CASE("Unclipped parent origin is part of the tracking command identity", "[clipped-parent]")
{
    Fixture fix;
    REQUIRE(fix.run().mode == Mode::Override);
    // A changed origin must discard the old update even if a producer has
    // not advanced its revision yet. Crop and clipped bounds are identical.
    fix.command.parentOriginX = -10;
    fix.exchange->select(fix.command);
    CHECK_FALSE(fix.exchange->fetch(fix.seen));
    fix.next(10.04);
    CHECK(fix.run().mode == Mode::Skip);
    CHECK(fix.detector.calls == 1);
}

TEST_CASE("Nonfinite parent origins are rejected before detection", "[clipped-parent]")
{
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        for (const bool horizontal : {false, true}) {
            Fixture fix;
            fix.command.parentOriginX = horizontal ? invalid : 0;
            fix.command.parentOriginY = horizontal ? 0 : invalid;
            fix.exchange->select(fix.command);
            CHECK(fix.run().mode == Mode::Skip);
            CHECK(fix.detector.calls == 0);
            CHECK_FALSE(fix.exchange->fetch(fix.seen));
        }
    }
}
