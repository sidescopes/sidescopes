#pragma once

#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "platform/desktop.h"

namespace sidescopes {

// Completion and expiry choose one winner under the same lock. A callback
// owns only this shared value, never the caller's stack or capture source.
template <typename T>
class CaptureCompletion
{
public:
    bool complete(T value)
    {
        const std::lock_guard lock(m_mutex);
        if (m_state != State::Pending) {
            return false;
        }
        m_value.emplace(std::move(value));
        m_state = State::Completed;
        m_ready.notify_one();
        return true;
    }

    std::optional<T> wait(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        try {
            if (!m_ready.wait_for(lock, timeout, [this] { return m_state != State::Pending; })) {
                m_state = State::Abandoned;
            }
        } catch (...) {
            // wait_for reacquires this lock before returning or throwing. An
            // exceptional wait must reject late publication just like expiry.
            m_state = State::Abandoned;
            throw;
        }
        auto value = std::move(m_value);
        m_value.reset();
        return value;
    }

    bool abandoned()
    {
        const std::lock_guard lock(m_mutex);
        return m_state == State::Abandoned;
    }

    void abandon()
    {
        const std::lock_guard lock(m_mutex);
        if (m_state == State::Pending) {
            m_state = State::Abandoned;
            m_ready.notify_one();
        }
    }

private:
    enum class State
    {
        Pending,
        Completed,
        Abandoned
    };
    std::mutex m_mutex;
    std::condition_variable m_ready;
    State m_state = State::Pending;
    std::optional<T> m_value;
};

class SckScreenCaptureSource;

struct SckCallbackState
{
    void retire()
    {
        const std::lock_guard lock(mutex);
        owner = nullptr;
    }

    std::mutex mutex;
    SckScreenCaptureSource* owner = nullptr;
};

enum class CaptureStartResult
{
    Started,
    Failed,
    TimedOut
};

// These bounds keep a lost daemon reply from blocking the application forever.
// Each operation has its own budget; native request submission itself is not
// interruptible. Cancellation after expiry is best effort, not an OS reset.
inline constexpr auto CaptureCompletionTimeout = std::chrono::seconds(5);

CaptureStartResult startCaptureWithDeadline(SCStream* stream, const std::shared_ptr<SckCallbackState>& callbacks,
                                            std::chrono::milliseconds timeout = CaptureCompletionTimeout);
bool stopCaptureWithDeadline(SCStream* stream, std::chrono::milliseconds timeout = CaptureCompletionTimeout);

using CaptureImageOwner = std::unique_ptr<std::remove_pointer_t<CGImageRef>, decltype(&CGImageRelease)>;
// Converts only on the caller after a successful wait, never on the SDK's
// completion queue. Allocation failure is an unavailable screenshot.
std::optional<CapturedImage> convertCaptureImage(CGImageRef image) noexcept;

}  // namespace sidescopes
