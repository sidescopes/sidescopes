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
struct FrameBuffer
{
    std::vector<uint8_t> data;
    int strideBytes = 0;
    int width = 0;
    int height = 0;
    ColorSpaceHint colorSpace = ColorSpaceHint::Unknown;
    uint64_t sequence = 0;

    [[nodiscard]] FrameView view() const
    {
        return FrameView{data.data(), strideBytes, width, height, colorSpace, sequence};
    }
};

// Single-slot mailbox between one producer (capture) and one consumer
// (analysis). The producer always publishes without blocking and overwrites
// any frame the consumer has not taken yet — the consumer only ever sees the
// newest frame, and nothing queues up when it falls behind. Buffers are
// recycled in both directions so the steady state allocates nothing.
class FrameMailbox
{
public:
    // Publishes a filled buffer and returns storage to reuse for the next
    // frame (possibly empty on the first exchanges). If the previous frame
    // was never taken, its storage is what comes back.
    FrameBuffer publish(FrameBuffer&& filled)
    {
        std::lock_guard lock(m_mutex);
        FrameBuffer reusable = std::move(m_returned);
        if (m_hasPending) {
            reusable = std::move(m_pending);
        }
        m_pending = std::move(filled);
        m_hasPending = true;
        m_available.notify_one();
        return reusable;
    }

    // Takes the newest frame if one arrived since the last take, waiting up
    // to `timeout` for it. A nudge ends the wait early without a frame.
    std::optional<FrameBuffer> takeLatest(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        m_available.wait_for(lock, timeout, [&] { return m_hasPending || m_nudged; });
        m_nudged = false;
        if (!m_hasPending) {
            return std::nullopt;
        }
        m_hasPending = false;
        return std::move(m_pending);
    }

    // Wakes a consumer blocked in TakeLatest without publishing a frame,
    // so a settings change can recompute the frame the consumer already
    // holds instead of waiting out the take's timeout.
    void nudge()
    {
        std::lock_guard lock(m_mutex);
        m_nudged = true;
        m_available.notify_one();
    }

    // Hands storage back for the producer to reuse.
    void returnStorage(FrameBuffer&& used)
    {
        std::lock_guard lock(m_mutex);
        m_returned = std::move(used);
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_available;
    FrameBuffer m_pending;
    FrameBuffer m_returned;
    bool m_hasPending = false;
    bool m_nudged = false;
};

}  // namespace sidescopes
