#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "core/frame_mailbox.h"
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
    while (lastSeen < Frames) {
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

}  // namespace sidescopes
