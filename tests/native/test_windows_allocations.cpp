#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "allocation_failure.h"
#include "core/diagnostics.h"
#include "platform/face_detection.h"
#include "platform/screen_capture.h"
#include "temp_file.h"

namespace sidescopes {
namespace {

using namespace std::chrono_literals;

template <typename Operation>
bool withFailedAllocation(Operation operation)
{
    bool threw = false;
    bool result = false;
    test::AllocationFailure failure(0);
    try {
        result = operation();
    } catch (...) {
        threw = true;
    }
    failure.disarm();
    REQUIRE(failure.failures() == 1);
    REQUIRE_FALSE(threw);
    return result;
}

template <typename Predicate>
bool waitUntil(Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

std::string readLog(const test::TempFile& log)
{
    std::ifstream input(log.path());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void exerciseFaceStartup(const test::TempFile& log)
{
    // This executable owns a fresh support-query cache. Failing the first
    // thread allocation must leave the next call able to launch the query.
    CHECK_FALSE(withFailedAllocation([] { return supportsFaceDetection(); }));
    REQUIRE(waitUntil([&] {
        (void)supportsFaceDetection();
        return readLog(log).find("face_support completed supported=") != std::string::npos;
    }));
    if (!supportsFaceDetection()) {
        WARN("Support-query retry passed; this Windows installation has no face detector");
        return;
    }

    constexpr int Edge = 128;
    const std::vector<uint8_t> pixels(static_cast<std::size_t>(Edge) * Edge * 4, 0);
    const FrameView frame{pixels.data(), Edge * 4, Edge, Edge};
    CHECK(withFailedAllocation([&] { return detectFaces(frame, 1.0f).empty(); }));
    CHECK(readLog(log).find("face_detection worker allocation failed") != std::string::npos);
    CHECK(detectFaces(frame, 1.0f).empty());
    CHECK(readLog(log).find("face_detection completed faces=0") != std::string::npos);
}

void exerciseCaptureStartup()
{
    FrameMailbox mailbox;
    auto source = createScreenCaptureSource();
    CaptureTarget target;
    // An impossible adapter exercises the real worker's error callback
    // without opening output duplication or reading the desktop.
    target.identifier = "4294967295:0";
    std::atomic<bool> notified{false};
    source->setStatusCallback([&](const std::string&) {
        notified.store(true);
        throw std::bad_alloc{};
    });
    CHECK_FALSE(withFailedAllocation([&] { return source->start(target, 15, mailbox); }));
    source->stop();
    notified.store(false);
    REQUIRE(source->start(target, 15, mailbox));
    const bool received = waitUntil([&] { return notified.load(); });
    source->stop();
    CHECK(received);
}

}  // namespace

TEST_CASE("Native Windows workers recover from startup allocation failures", "[native][allocation]")
{
    const test::TempFile log("native-worker-allocations.log");

    struct RecordingScope
    {
        ~RecordingScope()
        {
            diagConfigure({});
        }
    } recording;

    diagConfigure({"facelock,perf", log.path().string(), DiagFlush::EveryLine});
    exerciseFaceStartup(log);
    exerciseCaptureStartup();
}

}  // namespace sidescopes
