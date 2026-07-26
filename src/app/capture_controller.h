#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "core/frame_mailbox.h"
#include "platform/screen_capture.h"

namespace sidescopes {

/// How often the screen is read before anything asks for a different rate.
/// Measured whole-application over content changing continuously: 0.422 cores
/// at 30 a second, 0.336 at 20, 0.286 at 15, 0.220 at 10. Fifteen also costs
/// nothing at all on a large region - a whole display already only manages
/// 14.2 passes a second, and at 30 the other 52% of the frames captured are
/// thrown away unanalysed. What it costs is the step: during a brisk exposure
/// drag the waveform moves 0.9% of its pane's height between updates at 30 and
/// 1.8% at 15.
inline constexpr int DefaultCaptureFramesPerSecond = 15;

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

    /// Reads the screen @p framesPerSecond times a second from here on. The
    /// rate is fixed when a stream is created, so changing it replaces a
    /// running one; a stopped, suspended or dead stream is left alone, since
    /// the next start reads the new rate anyway.
    void setFrameRate(int framesPerSecond);

    /// Marks the stream stale so the next service() restarts it (system wake
    /// or unlock). Safe to call from any thread.
    void markStale();

    /// @return Whether the stream has died and awaits a restart.
    [[nodiscard]] bool dead() const;

    /// Stops capturing until resume(), because nothing is asking for frames.
    /// The whole pipeline goes with the stream: the backend's own work, the
    /// per-frame copy into the mailbox, change detection, and the analysis
    /// pass. The frames go too - a stopped stream holding a display's worth of
    /// pixels for deliveries that are not coming is the largest single thing
    /// this application keeps. service() leaves a suspended controller alone
    /// rather than reading the stopped stream as one that died.
    /// @p reason becomes the status line, since the two callers pause for
    /// different reasons and the settings window says which.
    void suspend(const std::string& reason);

    /// Starts capturing again after a suspend(). This start is also the
    /// restart a wake or unlock during the pause would have asked for, so the
    /// stale and dead marks are cleared rather than acted on twice.
    void resume();

    /// @return Whether capture is suspended.
    [[nodiscard]] bool suspended() const;

    /// Asks the stream to deliver only @p rect of the display, in display pixels,
    /// or the whole display when nothing is passed. Ignored while the pipeline is
    /// suspended or not running - a stream that is not delivering has nothing to
    /// narrow, and a restart begins on the whole display, so the next decision
    /// re-applies whatever is wanted.
    void narrowTo(const std::optional<IntRect>& rect);

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
    int m_frameRate = DefaultCaptureFramesPerSecond;
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
