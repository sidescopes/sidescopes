#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "core/frame_mailbox.h"
#include "platform/screen_capture.h"

namespace sidescopes {

/// Owns the screen-capture service: the permission verdict, which display is
/// captured versus desired, the status line, and the restart policy for a
/// stream that died or went stale. Frames keep flowing through the mailbox;
/// this owns control, not pixels.
class CaptureController
{
public:
    /// Binds to the capture @p source and the @p mailbox its frames land in,
    /// and registers the source's status callback. Both must outlive the
    /// controller.
    CaptureController(ScreenCaptureSource& source, FrameMailbox& mailbox);

    /// Asks the OS once; on denial the status explains how to grant it.
    bool requestPermission();

    /// @return Whether permission to capture the screen was granted.
    [[nodiscard]] bool permissionGranted() const;

    /// Starts capturing the desired display, or the first target when none is
    /// desired, stopping any running stream first. A desired display absent
    /// from the target list leaves the stream stopped and posts the
    /// disconnect status. Does nothing without permission.
    /// @return Whether a stream is running.
    bool start();

    /// @return The display a stream is capturing, or 0 when none is.
    [[nodiscard]] uint32_t capturedDisplay() const;

    /// @return The display the user chose to scope, held across restarts;
    /// 0 means whichever the backend lists first.
    [[nodiscard]] uint32_t desiredDisplay() const;

    /// Chooses the display the next start() captures.
    void requestDisplay(uint32_t displayId);

    /// Marks the stream stale so the next service() restarts it (system wake
    /// or unlock). Safe to call from any thread.
    void markStale();

    /// @return Whether the stream has died and awaits a restart.
    [[nodiscard]] bool dead() const;

    /// Stops capturing until resume(), because nothing is on screen to show
    /// it. The whole pipeline goes with the stream: the backend's own work,
    /// the per-frame copy into the mailbox, change detection, and the analysis
    /// pass. service() leaves a suspended controller alone rather than reading
    /// the stopped stream as one that died.
    void suspend();

    /// Starts capturing again after a suspend(). This start is also the
    /// restart a wake or unlock during the pause would have asked for, so the
    /// stale and dead marks are cleared rather than acted on twice.
    void resume();

    /// @return Whether capture is suspended.
    [[nodiscard]] bool suspended() const;

    /// Once per frame: consumes a stale mark, then restarts a dead stream
    /// after its backoff. @p now is the frame clock in seconds; the
    /// controller never reads the clock itself, so tests drive time here.
    void service(double now);

    /// @return The current capture status line.
    [[nodiscard]] std::string status() const;

private:
    void setStatus(const std::string& message);

    ScreenCaptureSource& m_source;
    FrameMailbox& m_mailbox;

    // The status callback lands on a backend thread while the frame loop
    // reads the line, so the string is mutex-guarded and death is an atomic.
    mutable std::mutex m_statusMutex;
    std::string m_status = "starting";
    std::atomic<bool> m_dead{false};
    // Set from the system-wake observer on any thread, consumed by service().
    std::atomic<bool> m_stale{false};

    uint32_t m_capturedDisplay = 0;
    uint32_t m_desiredDisplay = 0;
    bool m_permissionGranted = false;
    // Whether a stream is running, so start() stops the old one first only
    // when there is one; the first start has nothing to stop.
    bool m_running = false;
    // Whether the stream is stopped on purpose, which service() must not
    // mistake for a stream to revive.
    bool m_suspended = false;
    // The frame-clock deadline a dead stream waits out before a restart, and
    // how many restarts have failed in a row - the wait doubles with them, so a
    // lock or a display asleep for hours stops costing a retry every two
    // seconds. Reset by a successful start and by a wake, which changes the
    // conditions that were failing.
    double m_nextRetry = 0.0;
    int m_failedRestarts = 0;
};

}  // namespace sidescopes
