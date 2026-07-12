#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "core/frame_mailbox.h"

namespace sidescopes {
namespace {

using namespace std::chrono_literals;

FrameBuffer MakeFrame(uint64_t sequence, std::size_t bytes = 16) {
    FrameBuffer frame;
    frame.data.assign(bytes, static_cast<uint8_t>(sequence));
    frame.width = 2;
    frame.height = 2;
    frame.stride_bytes = 8;
    frame.sequence = sequence;
    return frame;
}

}  // namespace

TEST_CASE("FrameMailbox hands the newest frame to the consumer") {
    FrameMailbox mailbox;
    mailbox.Publish(MakeFrame(1));

    const auto taken = mailbox.TakeLatest(0ms);
    REQUIRE(taken.has_value());
    CHECK(taken->sequence == 1);

    SECTION("and nothing more until the next publish") {
        CHECK_FALSE(mailbox.TakeLatest(0ms).has_value());
    }
}

TEST_CASE("FrameMailbox overwrites an untaken frame") {
    FrameMailbox mailbox;
    mailbox.Publish(MakeFrame(1));
    mailbox.Publish(MakeFrame(2));

    const auto taken = mailbox.TakeLatest(0ms);
    REQUIRE(taken.has_value());
    CHECK(taken->sequence == 2);
    CHECK_FALSE(mailbox.TakeLatest(0ms).has_value());
}

TEST_CASE("FrameMailbox recycles storage in both directions") {
    FrameMailbox mailbox;

    // Overwriting returns the dropped frame's storage to the producer.
    mailbox.Publish(MakeFrame(1, 4096));
    const FrameBuffer reused = mailbox.Publish(MakeFrame(2));
    CHECK(reused.data.capacity() >= 4096);

    // Storage returned by the consumer comes back on the next publish.
    auto taken = mailbox.TakeLatest(0ms);
    REQUIRE(taken.has_value());
    taken->data.reserve(8192);
    mailbox.ReturnStorage(std::move(*taken));
    const FrameBuffer reused_again = mailbox.Publish(MakeFrame(3));
    CHECK(reused_again.data.capacity() >= 8192);
}

TEST_CASE("FrameMailbox timeout expires when no frame arrives") {
    FrameMailbox mailbox;
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(mailbox.TakeLatest(30ms).has_value());
    CHECK(std::chrono::steady_clock::now() - start >= 25ms);
}

TEST_CASE("FrameMailbox delivers across threads") {
    FrameMailbox mailbox;
    constexpr uint64_t kFrames = 200;

    std::thread producer([&] {
        for (uint64_t i = 1; i <= kFrames; ++i) {
            mailbox.Publish(MakeFrame(i));
            if (i % 16 == 0) std::this_thread::sleep_for(1ms);
        }
    });

    uint64_t last_seen = 0;
    while (last_seen < kFrames) {
        if (auto frame = mailbox.TakeLatest(100ms)) {
            CHECK(frame->sequence > last_seen);  // never stale, never repeated
            last_seen = frame->sequence;
            mailbox.ReturnStorage(std::move(*frame));
        }
    }
    producer.join();
    CHECK(last_seen == kFrames);
}

TEST_CASE("FrameMailbox nudge ends a take without a frame") {
    // A settings change must not wait out the take's timeout: the nudge
    // wakes the consumer immediately, empty-handed.
    FrameMailbox mailbox;
    mailbox.Nudge();
    const auto started = std::chrono::steady_clock::now();
    const auto taken = mailbox.TakeLatest(std::chrono::milliseconds(500));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK_FALSE(taken.has_value());
    CHECK(elapsed < std::chrono::milliseconds(200));
}

}  // namespace sidescopes
