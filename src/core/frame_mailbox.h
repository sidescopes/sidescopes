#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "core/frame.h"

namespace sidescopes {

// Owned frame storage passed between the capture and analysis threads.
struct FrameBuffer {
    std::vector<uint8_t> data;
    int stride_bytes = 0;
    int width = 0;
    int height = 0;
    ColorSpaceHint color_space = ColorSpaceHint::Unknown;
    uint64_t sequence = 0;

    [[nodiscard]] FrameView View() const {
        return FrameView{data.data(), stride_bytes, width, height, color_space, sequence};
    }
};

// Single-slot mailbox between one producer (capture) and one consumer
// (analysis). The producer always publishes without blocking and overwrites
// any frame the consumer has not taken yet — the consumer only ever sees the
// newest frame, and nothing queues up when it falls behind. Buffers are
// recycled in both directions so the steady state allocates nothing.
class FrameMailbox {
public:
    // Publishes a filled buffer and returns storage to reuse for the next
    // frame (possibly empty on the first exchanges). If the previous frame
    // was never taken, its storage is what comes back.
    FrameBuffer Publish(FrameBuffer&& filled) {
        std::lock_guard lock(mutex_);
        FrameBuffer reusable = std::move(returned_);
        if (has_pending_) reusable = std::move(pending_);
        pending_ = std::move(filled);
        has_pending_ = true;
        available_.notify_one();
        return reusable;
    }

    // Takes the newest frame if one arrived since the last take, waiting up
    // to `timeout` for it.
    std::optional<FrameBuffer> TakeLatest(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!available_.wait_for(lock, timeout, [&] { return has_pending_; })) return std::nullopt;
        has_pending_ = false;
        return std::move(pending_);
    }

    // Hands storage back for the producer to reuse.
    void ReturnStorage(FrameBuffer&& used) {
        std::lock_guard lock(mutex_);
        returned_ = std::move(used);
    }

private:
    std::mutex mutex_;
    std::condition_variable available_;
    FrameBuffer pending_;
    FrameBuffer returned_;
    bool has_pending_ = false;
};

}  // namespace sidescopes
