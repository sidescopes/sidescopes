#include "app/capture_controller.h"

#include <algorithm>
#include <cstdio>

namespace sidescopes {
namespace {

// How long a stream that is wanted but absent waits before the next attempt,
// and the ceiling that wait doubles up to. A fixed two-second retry runs
// eighteen hundred times an hour, which is worth avoiding for a screen that
// stays locked; doubling keeps a transient failure as quick to recover from as
// it ever was and settles a long one into a tick the machine can sleep
// through.
//
// The ceiling is a handful of seconds rather than the half-minute it was,
// because it is also the longest a user can be left looking at a page that
// says the capture is coming back. An attempt costs a display enumeration and
// nothing else - there are no frames to process while capture is broken - and
// the platforms that observe a lock suspend the pipeline outright, so they
// make no attempts at all. Paying seven hundred and twenty enumerations an
// hour on the platforms that do not is the better side of that trade.
//
// Five rather than a tuned value: nothing measured picks a number here, and a
// figure that looks calculated invites the next reader to hunt for the
// measurement behind it. The cost of being wrong is small in both directions.
constexpr double FirstRetrySeconds = 2.0;
constexpr double LongestRetrySeconds = 5.0;

// The wait after @p failures consecutive failed restarts, doubling from the
// first up to the ceiling.
double retryDelaySeconds(int failures)
{
    double delay = FirstRetrySeconds;
    for (int doubled = 1; doubled < failures && delay < LongestRetrySeconds; ++doubled) {
        delay *= 2.0;
    }

    return std::min(delay, LongestRetrySeconds);
}

}  // namespace

CaptureController::CaptureController(ScreenCaptureSource& source, FrameMailbox& mailbox)
    : m_source(source),
      m_mailbox(mailbox)
{
    // Capture is a service that can die at any time on any thread (lock
    // screen, display sleep, a monitor unplugged); the callback records why and
    // withdraws the stream, which is all service() needs to start building
    // another.
    m_source.setStatusCallback([this](const std::string& message) {
        setStatus(message);
        m_streamAlive.store(false);
    });
}

bool CaptureController::requestPermission()
{
    m_permissionGranted = m_source.requestPermission() == CapturePermission::Granted;
    if (!m_permissionGranted) {
        setStatus(
            "screen recording permission missing - grant it in System Settings and "
            "relaunch");
    }

    return m_permissionGranted;
}

bool CaptureController::permissionGranted() const
{
    return m_permissionGranted;
}

bool CaptureController::captureWindow()
{
    if (!m_permissionGranted || m_suspended || !m_source.supportsWindowCapture()) {
        return false;
    }
    if (m_running) {
        m_source.stop();
        m_running = false;
        m_streamAlive.store(false);
    }
    m_windowMode = true;
    m_capturedDisplay = 0;
    m_desiredDisplay = 0;
    if (!m_source.startWindowCapture(m_frameRate, m_mailbox)) {
        m_windowMode = false;

        return false;
    }
    // Optimistic like start(): the compositor's pick may still be open. A
    // decline or a failure arrives through the status callback, which drops
    // m_streamAlive, and service() then ends window mode.
    m_streamAlive.store(true);
    m_running = true;
    setStatus("choose a window to scope");

    return true;
}

bool CaptureController::capturingWindow() const
{
    return m_windowMode;
}

bool CaptureController::windowCaptureSupported() const
{
    return m_source.supportsWindowCapture();
}

const CaptureTarget* CaptureController::chooseTarget(const std::vector<CaptureTarget>& targets)
{
    if (targets.empty()) {
        // No display at all, which is what a session on its way to sleep and a
        // sole monitor unplugged both look like from here. Naming it keeps the
        // status line from standing on whatever was true before.
        setStatus("no display available - scopes resume when one returns");

        return nullptr;
    }
    if (m_desiredDisplay == 0) {
        return &targets.front();
    }
    const auto wanted = std::find_if(targets.begin(), targets.end(), [&](const CaptureTarget& candidate) {
        return candidate.displayId == m_desiredDisplay;
    });
    if (wanted == targets.end()) {
        // The chosen display is disconnected. The scopes pause on the banner
        // rather than silently jumping to another screen; the retry loop
        // resumes the same region the moment it returns.
        setStatus("display disconnected - scopes resume when it returns");

        return nullptr;
    }

    return &*wanted;
}

bool CaptureController::start()
{
    // Any display start leaves window mode: the two are exclusive, and this is
    // the single place the monitor path reclaims the controller, so nothing
    // else has to remember to clear it.
    m_windowMode = false;

    // A suspended pipeline is stopped because nothing on screen is asking for
    // frames. Starting one here would defeat the pause and leave a live stream
    // behind a controller that services nothing - and that stream's eventual
    // death is a mark no suspended service() can act on.
    if (!m_permissionGranted || m_suspended) {
        return false;
    }

    // Every exit below leaves the controller wanting a stream and holding none,
    // which is the state service() retries out of. Nothing has to be marked on
    // the way out, and no exit can forget to.
    if (m_running) {
        m_source.stop();
        m_running = false;
        m_streamAlive.store(false);
    }

    const auto targets = m_source.listTargets();
    const CaptureTarget* target = chooseTarget(targets);
    if (target == nullptr) {
        return false;
    }

    if (!m_source.start(*target, m_frameRate, m_mailbox)) {
        return false;
    }

    m_capturedDisplay = target->displayId;
    m_desiredDisplay = target->displayId;
    setStatus("capturing " + target->description);
    m_streamAlive.store(true);
    m_running = true;

    return true;
}

uint32_t CaptureController::capturedDisplay() const
{
    return m_capturedDisplay;
}

uint32_t CaptureController::desiredDisplay() const
{
    return m_desiredDisplay;
}

void CaptureController::requestDisplay(uint32_t displayId)
{
    // Choosing a display leaves window scope: the next start() captures the
    // monitor, not the window.
    m_windowMode = false;
    m_desiredDisplay = displayId;
}

void CaptureController::setFrameRate(int framesPerSecond)
{
    const int wanted = std::max(framesPerSecond, 1);
    if (wanted == m_frameRate) {
        return;
    }
    m_frameRate = wanted;
    // Photography is the subject, and a scope carries a distribution rather
    // than motion, so this is a choice about how smoothly the trace reads and
    // not about what the display can deliver. The rate is fixed when a stream
    // is created, so a running one is replaced to pick the new one up.
    if (m_running && !m_suspended) {
        (void)start();
    }
}

void CaptureController::markStale()
{
    m_stale.store(true);
}

bool CaptureController::dead() const
{
    // Read off the facts rather than remembered: capture is wanted, and there
    // is no stream serving it. Nothing has to set this and nothing has to
    // clear it, so no path through the controller can strand the application
    // in a failure it is not working on - or on a page claiming a
    // reconnection while the pipeline is deliberately stopped.
    return m_permissionGranted && !m_suspended && !(m_running && m_streamAlive.load());
}

void CaptureController::suspend(const std::string& reason)
{
    if (m_suspended) {
        return;
    }
    m_suspended = true;
    m_pauseReason = reason;
    if (m_running) {
        m_source.stop();
        m_running = false;
    }
    m_streamAlive.store(false);
    // The producer has stopped by here, so its buffers are nobody's: freeing
    // them is what turns a suspend into a memory saving rather than only a
    // CPU one.
    m_mailbox.release();
    setStatus(reason);
}

void CaptureController::resume()
{
    if (!m_suspended) {
        return;
    }
    m_suspended = false;
    m_stale.store(false);
    // Frames are wanted again from a clean sheet: whatever failures the last
    // stream earned belong to conditions that have had every chance to change,
    // and a start that fails here leaves the controller wanting a stream with
    // none - which is the state service() retries out of, with no mark to set.
    m_failedRestarts = 0;
    m_nextRetry = 0.0;
    (void)start();
}

void CaptureController::narrowTo(const std::optional<IntRect>& rect)
{
    if (!m_running || m_suspended) {
        return;
    }
    m_source.narrowTo(rect);
}

bool CaptureController::suspended() const
{
    return m_suspended;
}

void CaptureController::service(double now)
{
    // A suspended pipeline is stopped because nothing on screen is asking for
    // frames, so there is no stream to rebuild and no failure to report. The
    // reason is restated rather than remembered as having been posted: the
    // backend reports a stop from its own thread, and a stop this controller
    // asked for can land after the pause, overwriting the line that says why
    // the pipeline is paused. Restating it every tick cannot be forgotten and
    // cannot be consumed by the wrong caller. The stale mark keeps until
    // resume(), whose start is the restart it asks for.
    if (m_suspended) {
        setStatus(m_pauseReason);

        return;
    }
    if (m_windowMode) {
        // A window stream that ends means the window closed or the pick was
        // cancelled. The compositor's picker must not re-pop on a retry, so
        // window scope ENDS rather than rebuilding: the application sees
        // capture stop and clears the window-scoped region. This branch keeps
        // window mode entirely off the monitor retry path below - which is
        // therefore unchanged whenever a window is not being scoped.
        if (!(m_running && m_streamAlive.load())) {
            m_windowMode = false;
            m_running = false;
            setStatus("window scope ended");
        }

        return;
    }
    // Waking the display or unlocking the session can leave the stream a
    // zombie: it either stops delivering without an error, or a retry that
    // ran while the screen was locked bound a stream to the wrong session.
    // Both look alive, so the wake signal withdraws the stream outright -
    // cheap on a screen that was just black.
    if (m_stale.exchange(false)) {
        std::fprintf(stderr, "sidescopes: restarting capture after wake or unlock\n");
        m_streamAlive.store(false);
        // Give the session a moment to finish coming back.
        m_nextRetry = now + 1.0;
        // Conditions just changed, so whatever had been failing deserves a
        // prompt attempt rather than the backoff the last failures earned.
        m_failedRestarts = 0;
    }
    if (dead() && now > m_nextRetry) {
        if (start()) {
            m_failedRestarts = 0;
        } else {
            ++m_failedRestarts;
            m_nextRetry = now + retryDelaySeconds(m_failedRestarts);
        }
    }
}

std::string CaptureController::status() const
{
    std::lock_guard lock(m_statusMutex);

    return m_status;
}

void CaptureController::setStatus(const std::string& message)
{
    std::lock_guard lock(m_statusMutex);
    m_status = message;
}

}  // namespace sidescopes
