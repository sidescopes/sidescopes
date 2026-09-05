#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#include "core/diagnostics.h"
#include "core/frame_mailbox.h"
#include "temp_file.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

namespace {

using namespace std::chrono_literals;

// The mailbox only reads a frame's sequence and storage, so its frames are
// the smallest solid buffers - a 2x2 is 16 bytes at four channels, matching
// the sizes these tests assert on. The color is immaterial here.
FrameBuffer makeFrame(uint64_t sequence, int side = 2)
{
    return makeSolidFrameBuffer(side, side, Color{}, sequence);
}

// The milliseconds carried by the first "capture interval_ms=" line in a log,
// or -1 when there is none.
double firstIntervalMs(const std::string& path)
{
    static constexpr std::string_view Key = "capture interval_ms=";
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t at = line.find(Key);
        if (at != std::string::npos) {
            return std::stod(line.substr(at + Key.size()));
        }
    }

    return -1.0;
}

}  // namespace

TEST_CASE("FrameMailbox hands the newest frame to the consumer")
{
    FrameMailbox mailbox;
    mailbox.publish(makeFrame(1));

    const auto taken = mailbox.takeLatest(0ms);
    REQUIRE(taken.has_value());
    CHECK(taken->sequence == 1);

    SECTION("and nothing more until the next publish")
    {
        CHECK_FALSE(mailbox.takeLatest(0ms).has_value());
    }
}

TEST_CASE("FrameMailbox overwrites an untaken frame")
{
    FrameMailbox mailbox;
    mailbox.publish(makeFrame(1));
    mailbox.publish(makeFrame(2));

    const auto taken = mailbox.takeLatest(0ms);
    REQUIRE(taken.has_value());
    CHECK(taken->sequence == 2);
    CHECK_FALSE(mailbox.takeLatest(0ms).has_value());
}

TEST_CASE("FrameMailbox recycles storage in both directions")
{
    FrameMailbox mailbox;

    // Overwriting returns the dropped frame's storage to the producer. The
    // 32x32 buffer is 4096 bytes; the tiny 2x2 that overwrites it inherits
    // that larger capacity.
    mailbox.publish(makeFrame(1, 32));
    const FrameBuffer reused = mailbox.publish(makeFrame(2));
    CHECK(reused.data.capacity() >= 4096);

    // Storage returned by the consumer comes back on the next publish.
    auto taken = mailbox.takeLatest(0ms);
    REQUIRE(taken.has_value());
    taken->data.reserve(8192);
    mailbox.returnStorage(std::move(*taken));
    const FrameBuffer reusedAgain = mailbox.publish(makeFrame(3));
    CHECK(reusedAgain.data.capacity() >= 8192);
}

TEST_CASE("FrameMailbox timeout expires when no frame arrives")
{
    FrameMailbox mailbox;
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(mailbox.takeLatest(30ms).has_value());
    CHECK(std::chrono::steady_clock::now() - start >= 25ms);
}

TEST_CASE("FrameMailbox delivers across threads")
{
    FrameMailbox mailbox;
    constexpr uint64_t Frames = 200;

    std::thread producer([&] {
        for (uint64_t i = 1; i <= Frames; ++i) {
            mailbox.publish(makeFrame(i));
            if (i % 16 == 0) {
                std::this_thread::sleep_for(1ms);
            }
        }
    });

    uint64_t lastSeen = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (lastSeen < Frames && std::chrono::steady_clock::now() < deadline) {
        if (auto frame = mailbox.takeLatest(100ms)) {
            CHECK(frame->sequence > lastSeen);  // never stale, never repeated
            lastSeen = frame->sequence;
            mailbox.returnStorage(std::move(*frame));
        }
    }
    producer.join();
    CHECK(lastSeen == Frames);
}

TEST_CASE("FrameMailbox nudge ends a take without a frame")
{
    // A settings change must not wait out the take's timeout: the nudge
    // wakes the consumer immediately, empty-handed.
    FrameMailbox mailbox;
    mailbox.nudge();
    const auto started = std::chrono::steady_clock::now();
    const auto taken = mailbox.takeLatest(std::chrono::milliseconds(500));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK_FALSE(taken.has_value());
    CHECK(elapsed < std::chrono::milliseconds(200));
}

TEST_CASE("FrameMailbox nudge does not swallow a pending frame")
{
    // A nudge riding alongside a real frame must still hand the frame over,
    // not consume the wake and drop it.
    FrameMailbox mailbox;
    mailbox.publish(makeFrame(7));
    mailbox.nudge();

    const auto taken = mailbox.takeLatest(0ms);
    REQUIRE(taken.has_value());
    CHECK(taken->sequence == 7);
}

TEST_CASE("FrameBuffer holds no more pixels than the frame needs")
{
    // The capture narrows to the region once it settles, so a buffer that
    // carried a whole display must not keep those pages: three of them at a
    // display each is what the pipeline used to hold for a region a fraction
    // of the size.
    FrameBuffer buffer;
    buffer.sizeTo(static_cast<std::size_t>(4096) * 2160 * 4);
    const std::size_t whole = buffer.data.capacity();
    REQUIRE(whole >= static_cast<std::size_t>(4096) * 2160 * 4);

    buffer.sizeTo(static_cast<std::size_t>(640) * 480 * 4);
    const std::size_t narrowed = buffer.data.capacity();
    CHECK(buffer.data.size() == static_cast<std::size_t>(640) * 480 * 4);
    CHECK(narrowed < whole / 4);
}

TEST_CASE("FrameBuffer keeps its pixels where the size does not move")
{
    // The steady state must not reallocate: a delivery of the size already
    // held is the ordinary case, thirty times a second.
    FrameBuffer buffer;
    buffer.sizeTo(static_cast<std::size_t>(1920) * 1080 * 4);
    const std::uint8_t* first = buffer.data.data();
    buffer.data[7] = 42;

    buffer.sizeTo(static_cast<std::size_t>(1920) * 1080 * 4);
    CHECK(buffer.data.data() == first);
    CHECK(buffer.data[7] == 42);
}

TEST_CASE("FrameMailbox spends a nudge on a single take")
{
    // The first take consumes the nudge; the next take, with nothing
    // pending, must wait out its full timeout rather than return early on a
    // stale nudge.
    FrameMailbox mailbox;
    mailbox.nudge();
    CHECK_FALSE(mailbox.takeLatest(0ms).has_value());  // consumes the nudge

    const auto started = std::chrono::steady_clock::now();
    const auto taken = mailbox.takeLatest(50ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK_FALSE(taken.has_value());
    CHECK(elapsed >= 45ms);  // waited its timeout, generous margin
}

TEST_CASE("The first interval of a recording is not the gap since the last one")
{
    const test::TempFile first("mailbox-cadence-first.log");
    const test::TempFile second("mailbox-cadence-second.log");
    FrameMailbox mailbox;

    diagConfigure({"perf", first.path().string()});
    mailbox.publish(makeFrame(1));  // the baseline, no line
    std::this_thread::sleep_for(5ms);
    mailbox.publish(makeFrame(2));
    diagConfigure({});

    // The pause between the two recordings: nothing publishes, and the
    // mailbox outlives both.
    std::this_thread::sleep_for(300ms);

    diagConfigure({"perf", second.path().string()});
    mailbox.publish(makeFrame(3));  // must be a baseline again
    std::this_thread::sleep_for(5ms);
    mailbox.publish(makeFrame(4));
    diagConfigure({});

    const double interval = firstIntervalMs(second.path().string());
    // Everything about the cadence is generous here except the one thing
    // asserted: a baseline left over from the first recording reports the
    // whole 300 ms pause as a capture interval.
    CHECK(interval >= 0.0);
    CHECK(interval < 100.0);
}

}  // namespace sidescopes
