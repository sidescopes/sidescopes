#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

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
};

/// Why open() came back empty. Declined is the user's answer and must not be
/// asked again this session; everything else is the environment's.
enum class PortalError
{
    None,
    Declined,
    Unavailable
};

/// What the session captures: a whole monitor, or a single window the
/// compositor's own picker chooses. A window stream IS that window and follows
/// it, which is how a native Wayland window - one with no queryable screen
/// rectangle - is attached to.
enum class PortalSourceKind
{
    Monitor,
    Window
};

/// The portal's `types` bitmask for a source kind: MONITOR (1) or WINDOW (2),
/// from the ScreenCast interface. Pure so the mapping is pinned by a test - a
/// wrong value asks the compositor for the wrong thing silently.
[[nodiscard]] uint32_t portalSourceTypeMask(PortalSourceKind kind);

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
