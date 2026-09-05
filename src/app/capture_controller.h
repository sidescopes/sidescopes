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
///
/// ONE INVARIANT CARRIES THE RECOVERY: while capture is wanted and no stream
/// is serving it, service() is building another. It is stated over the
/// controller's own two facts - whether a stream was created, and whether the
/// backend still calls it alive - so no caller has to record a failure and no
/// caller can record one nothing will act on. The alternative, a mark set
/// wherever capture is known to break, needs the application to know every way
/// it can break; this needs it to know only that it is broken, which it
/// observes directly.
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
    /// disconnect status. Does nothing without permission, and nothing while
    /// suspended: a paused pipeline must not acquire a stream behind the
    /// visibility gate's back, and the display asked for meanwhile is
    /// remembered, so resume() starts on it.
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

    /// Marks the stream stale so the next service() restarts it (system wake,
    /// unlock, or a monitor connected or disconnected). Safe to call from any
    /// thread. It is a responsiveness signal only - it says the conditions
    /// that were failing may have changed, so the schedule those failures
    /// earned is dropped - and recovery does not depend on one arriving.
    void markStale();

    /// @return Whether capture is wanted and no stream is serving it, which is
    /// exactly the state service() is retrying out of. A paused pipeline is
    /// not dead: it has asked for no stream.
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
    /// stale and dead marks are cleared rather than acted on twice. A start
    /// that fails here leaves the stream marked dead, since nothing was
    /// running to report the failure and service() is what tries again.
    void resume();

    /// @return Whether capture is suspended.
    [[nodiscard]] bool suspended() const;

    /// Asks the stream to deliver only @p rect of the display, in display pixels,
    /// or the whole display when nothing is passed. Ignored while the pipeline is
    /// suspended or not running - a stream that is not delivering has nothing to
    /// narrow, and a restart begins on the whole display, so the next decision
    /// re-applies whatever is wanted.
    void narrowTo(const std::optional<IntRect>& rect);

    /// Once per frame, and the whole of the recovery mechanism: while capture
    /// is wanted and no stream is serving it, another is built, on a backoff
    /// that no failure can push past a handful of seconds. It asks nothing
    /// about why the last one stopped - a stream reported dead, a start that
    /// failed, a display that went away and a wake all leave the same state
    /// behind, and this acts on the state rather than on the reason. While
    /// suspended it restates the pause instead. @p now is the frame clock in
    /// seconds; the controller never reads the clock itself, so tests drive
    /// time here.
    void service(double now);

    /// @return The current capture status line.
    [[nodiscard]] std::string status() const;

private:
    [[nodiscard]] bool startTarget(const CaptureTarget& target);
    void setStatus(const std::string& message);

    ScreenCaptureSource& m_source;
    FrameMailbox& m_mailbox;

    // The status callback lands on a backend thread while the frame loop
    // reads the line, so the string is mutex-guarded and the stream's health
    // is an atomic. That health is a fact the backend reports, not a mark to
    // be consumed: dead() is derived from it, so a failure cannot be recorded
    // in a state nothing will act on.
    mutable std::mutex m_statusMutex;
    std::string m_status = "starting";
    std::atomic<bool> m_streamAlive{false};
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
    // mistake for a stream to revive, and the line that says why - restored
    // when a stopped stream's own report of stopping lands after the pause.
    bool m_suspended = false;
    std::string m_pauseReason;
    // The frame-clock deadline the next attempt waits out, and how many
    // attempts have failed in a row - the wait doubles with them, so a lock or
    // a display asleep for hours stops costing a retry every two seconds.
    // Reset by a successful start, by a resume, and by a wake or a display
    // change, all of which change the conditions that were failing.
    double m_nextRetry = 0.0;
    int m_failedRestarts = 0;
};

}  // namespace sidescopes
