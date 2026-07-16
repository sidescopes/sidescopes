#include "app/capture_controller.h"

#include <algorithm>
#include <cstdio>

namespace sidescopes {
namespace {

// Capture runs at the display refresh cap the app has always requested.
constexpr int CaptureFramesPerSecond = 30;

}  // namespace

CaptureController::CaptureController(ScreenCaptureSource& source, FrameMailbox& mailbox)
    : m_source(source),
      m_mailbox(mailbox)
{
    // Capture is a service that can die at any time on any thread (lock
    // screen, display sleep); the callback records why and marks the stream
    // dead for the frame loop to restart.
    m_source.setStatusCallback([this](const std::string& message) {
        setStatus(message);
        m_dead.store(true);
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

bool CaptureController::start()
{
    if (!m_permissionGranted) {
        return false;
    }

    // A stream that was running but could not be re-established counts as
    // dead, so service() keeps retrying it; a first start with nothing yet
    // running stays quietly stopped on failure, as it always has.
    const bool wasRunning = m_running;
    if (m_running) {
        m_source.stop();
        m_running = false;
    }
    const auto fail = [&]() -> bool {
        if (wasRunning) {
            m_dead.store(true);
        }

        return false;
    };

    const auto targets = m_source.listTargets();
    if (targets.empty()) {
        return fail();
    }

    const CaptureTarget* target = &targets.front();
    if (m_desiredDisplay != 0) {
        const auto wanted = std::find_if(targets.begin(), targets.end(), [&](const CaptureTarget& candidate) {
            return candidate.displayId == m_desiredDisplay;
        });
        if (wanted == targets.end()) {
            // The chosen display is disconnected. The scopes pause on the
            // banner rather than silently jumping to another screen; the
            // retry loop resumes the same region the moment it returns.
            setStatus("display disconnected - scopes resume when it returns");

            return fail();
        }
        target = &*wanted;
    }

    if (!m_source.start(*target, CaptureFramesPerSecond, m_mailbox)) {
        return fail();
    }

    m_capturedDisplay = target->displayId;
    m_desiredDisplay = target->displayId;
    setStatus("capturing " + target->description);
    m_dead.store(false);
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
    m_desiredDisplay = displayId;
}

void CaptureController::markStale()
{
    m_stale.store(true);
}

bool CaptureController::dead() const
{
    return m_dead.load();
}

void CaptureController::service(double now)
{
    // Waking the display or unlocking the session can leave the stream a
    // zombie: it either stops delivering without an error, or a retry that
    // ran while the screen was locked bound a stream to the wrong session.
    // Both look alive, so the wake signal forces a restart - cheap on a
    // screen that was just black.
    if (m_stale.exchange(false)) {
        std::fprintf(stderr, "sidescopes: restarting capture after wake or unlock\n");
        m_dead.store(true);
        // Give the session a moment to finish coming back.
        m_nextRetry = now + 1.0;
    }
    if (m_permissionGranted && m_dead.load() && now > m_nextRetry) {
        if (!start()) {
            m_nextRetry = now + 2.0;
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
