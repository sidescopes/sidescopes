#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "platform/linux/portal_options.h"

struct DBusConnection;

namespace sidescopes {

/// What a completed portal handshake hands over: the PipeWire socket, the
/// node carrying the chosen source, and the single-use token that restores
/// this choice silently next time. The caller owns the descriptor.
struct PortalStream
{
    int pipewireFd = -1;
    uint32_t nodeId = 0;
    std::string restoreToken;
    /// Where the chosen source sits on the desktop and what it covers, as the
    /// stream's own properties state it. Both are optional in the portal spec:
    /// zero means the portal did not say, and the frame then speaks for its own
    /// space - right for every unscaled single-output session. Carried because
    /// the cursor position the stream reports is stated in FRAME pixels and has
    /// to become a desktop point.
    double originX = 0.0;
    double originY = 0.0;
    double widthPoints = 0.0;
    double heightPoints = 0.0;
};

/// Why open() came back empty. Declined is the user's answer and must not be
/// asked again this session; everything else is the environment's.
enum class PortalError
{
    None,
    Declined,
    Unavailable
};

/// One org.freedesktop.portal.ScreenCast session over its own D-Bus
/// connection: CreateSession, SelectSources, Start (the consent dialog,
/// skipped when a restore token still holds), OpenPipeWireRemote. Single
/// threaded by design - the capture thread owns the instance outright.
class PortalScreenCast
{
public:
    ~PortalScreenCast();

    /// Runs the whole handshake, blocking through the consent dialog.
    /// @p abort is polled while waiting so a closing application never
    /// hangs on an unanswered dialog. @p kind selects a monitor or a window
    /// source. On failure @p error names the reason.
    std::optional<PortalStream> open(PortalSourceKind kind, const std::string& restoreToken,
                                     const std::atomic<bool>& abort, PortalError& error);

    /// Dispatches queued D-Bus traffic for up to @p timeoutMs, watching for
    /// the session's Closed signal. False once the session died or the
    /// connection dropped - the capture stream is over either way.
    [[nodiscard]] bool pump(int timeoutMs);

    /// Closes the session politely and drops the connection.
    void close();

private:
    DBusConnection* m_connection = nullptr;
    std::string m_sessionHandle;
    bool m_sessionClosed = false;
};

}  // namespace sidescopes
