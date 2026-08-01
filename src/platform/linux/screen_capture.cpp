// Screen capture on Linux: the ScreenCast portal feeding a PipeWire stream.
// The targets are the connected outputs, so the interface speaks true
// display names; which output the stream actually carries is the portal
// dialog's answer, made silent on later launches by a persisted restore
// token. The whole bring-up runs on a capture thread - the consent dialog
// can sit open for minutes and the frame loop must keep drawing behind it.

#include "platform/screen_capture.h"

#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "platform/desktop.h"
#include "platform/linux/linux_session.h"
#include "platform/linux/pipewire_stream.h"
#include "platform/linux/portal_screencast.h"
#include "platform/linux/x11_displays.h"
#include "platform/linux/x11_shm_capture.h"

namespace sidescopes {
namespace {

/// The restore token lives beside the preferences: platform state owned by
/// the capture backend, deliberately not a preference the application sees.
std::string restoreTokenPath()
{
    const std::string preferences = preferencesFilePath();
    const std::size_t slash = preferences.find_last_of('/');
    if (slash == std::string::npos) {
        return "portal-restore-token";
    }
    return preferences.substr(0, slash + 1) + "portal-restore-token";
}

std::string loadRestoreToken()
{
    std::string token;
    std::FILE* file = std::fopen(restoreTokenPath().c_str(), "r");
    if (file == nullptr) {
        return token;
    }
    char buffer[512];
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    std::fclose(file);
    token.assign(buffer, read);
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
        token.pop_back();
    }
    return token;
}

/// Tokens are single-use: every Start hands back a fresh one, so failing to
/// persist it here costs a consent dialog on the next launch.
void saveRestoreToken(const std::string& token)
{
    const std::string path = restoreTokenPath();
    if (token.empty()) {
        std::remove(path.c_str());
        return;
    }
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        mkdir(path.substr(0, slash).c_str(), 0700);
    }
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    std::fwrite(token.data(), 1, token.size(), file);
    std::fclose(file);
}

class LinuxScreenCapture final : public ScreenCaptureSource
{
public:
    ~LinuxScreenCapture() override
    {
        stop();
    }

    CapturePermission requestPermission() override
    {
        // Consent is asked by the portal dialog at stream start, not granted
        // ahead of it; there is nothing to request here.
        return CapturePermission::Granted;
    }

    std::vector<CaptureTarget> listTargets() override
    {
        std::vector<CaptureTarget> targets;
        for (const LinuxDisplay& display : connectedDisplays()) {
            CaptureTarget target;
            target.identifier = std::to_string(display.id);
            target.description = display.name;
            target.displayId = display.id;
            target.widthPoints = static_cast<int>(display.geometry.widthPoints);
            target.heightPoints = static_cast<int>(display.geometry.heightPoints);
            targets.push_back(target);
        }
        return targets;
    }

    bool start(const CaptureTarget&, int maxFramesPerSecond, FrameMailbox& mailbox) override
    {
        return startKind(PortalSourceKind::Monitor, maxFramesPerSecond, mailbox);
    }

    bool supportsWindowCapture() const override
    {
        return true;
    }

    bool startWindowCapture(int maxFramesPerSecond, FrameMailbox& mailbox) override
    {
        // A window pick is always the compositor's own dialog - the restore
        // token, which pins a monitor, would silently reopen the last monitor
        // instead of letting the user choose a window - so it is not carried.
        return startKind(PortalSourceKind::Window, maxFramesPerSecond, mailbox);
    }

    void stop() override
    {
        m_stopRequested.store(true);
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void setStatusCallback(StatusCallback callback) override
    {
        m_status = std::move(callback);
    }

private:
    void report(const std::string& message)
    {
        if (m_status) {
            m_status(message);
        }
    }

    /// Spawns the capture thread for a monitor or a window source. Shared by
    /// both entry points; the kind only changes which source the handshake
    /// asks for and whether a restore token is offered.
    bool startKind(PortalSourceKind kind, int maxFramesPerSecond, FrameMailbox& mailbox)
    {
        if (m_declined) {
            report("screen sharing was declined - relaunch to be asked again");
            return false;
        }
        stop();
        m_stopRequested.store(false);
        m_worker = std::thread(
            [this, kind, maxFramesPerSecond, &mailbox] { captureSession(kind, maxFramesPerSecond, mailbox); });
        // Optimistic by design: the consent dialog may still be open. A
        // failure past this point arrives through the status callback, the
        // same way any stream death does.
        return true;
    }

    /// The capture thread: handshake, stream, then a watch on the portal
    /// session until asked to stop or the session dies under us.
    void captureSession(PortalSourceKind kind, int maxFramesPerSecond, FrameMailbox& mailbox)
    {
        PortalScreenCast portal;
        PortalError error = PortalError::None;
        // A window source never carries the monitor restore token.
        const std::string restoreToken = kind == PortalSourceKind::Monitor ? loadRestoreToken() : std::string();
        const std::optional<PortalStream> opened = portal.open(kind, restoreToken, m_stopRequested, error);
        if (!opened) {
            if (m_stopRequested.load()) {
                return;
            }
            if (error == PortalError::Declined) {
                // A declined window pick is not a standing refusal - the user
                // may pick one next time - so only a declined MONITOR share
                // latches m_declined and clears the token.
                if (kind == PortalSourceKind::Monitor) {
                    m_declined = true;
                    saveRestoreToken("");
                }
                report("screen sharing was declined - relaunch to be asked again");
            } else {
                report("screen sharing is unavailable - the desktop portal did not answer");
            }
            return;
        }
        // Only a monitor share persists a restore token; a window is always the
        // compositor's fresh pick.
        if (kind == PortalSourceKind::Monitor) {
            saveRestoreToken(opened->restoreToken);
        }

        PipeWireVideoStream stream;
        std::atomic<bool> streamDied{false};
        const bool started =
            stream.start(*opened, maxFramesPerSecond, mailbox, [this, &streamDied](const std::string& message) {
                streamDied.store(true);
                report(message);
            });
        if (!started) {
            report("capture stream could not start");
            return;
        }
        while (!m_stopRequested.load() && !streamDied.load()) {
            if (!portal.pump(200)) {
                if (!m_stopRequested.load()) {
                    report("capture session closed");
                }
                break;
            }
        }
        stream.stop();
    }

    std::thread m_worker;
    std::atomic<bool> m_stopRequested{false};
    bool m_declined = false;
    StatusCallback m_status;
};

}  // namespace

std::unique_ptr<ScreenCaptureSource> createScreenCaptureSource()
{
    // A pure X11 session reads the real screen with XShm directly - no portal,
    // no PipeWire, no consent dialog, the same shape as macOS and Windows. A
    // Wayland session cannot: XShm under XWayland sees only X clients, so the
    // ScreenCast portal is the only route to native Wayland pixels, and it is
    // also where the window-picker attach fallback lives.
    if (runningOnX11Session()) {
        return createX11ShmScreenCaptureSource();
    }
    return std::make_unique<LinuxScreenCapture>();
}

}  // namespace sidescopes
