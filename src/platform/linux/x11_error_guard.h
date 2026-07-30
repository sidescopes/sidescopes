#pragma once

#include <X11/Xlib.h>

namespace sidescopes {

/// Protocol errors raised on THIS thread since the live guard was opened.
/// Per-thread because the handler is process-wide while the connections are
/// not: one thread's errors must never be read as another's. An inline
/// thread_local gives one instance per thread across every translation unit
/// that includes this header.
inline thread_local unsigned int g_x11ErrorCount = 0;

inline int recordX11Error(Display*, XErrorEvent*)
{
    ++g_x11ErrorCount;

    return 0;
}

/// Installed once for the process and never restored: the default handler
/// EXITS, and an X client that addresses a window which died between being
/// listed and being asked about - or a server that refuses a shared-memory
/// attach - is ordinary rather than fatal. Non-fatal process-wide means no
/// stray protocol error from any thread can take the application down, which
/// is why installing it also makes every XShm call safe.
inline void ensureX11ErrorHandler()
{
    [[maybe_unused]] static const XErrorHandler previous = XSetErrorHandler(&recordX11Error);
}

/// Scopes a group of requests that may address a resource which has already
/// died, or that the server may refuse. X11 errors are asynchronous - a
/// BadWindow or BadAccess arrives long after the call that caused it returned -
/// so failed() syncs the connection before answering. The handler it relies on
/// is non-fatal and shared by every thread, so no swap and no restore: two
/// threads guarding their own connections never contend on a global handler
/// pointer, each reading only its own thread_local count.
class X11ErrorGuard
{
public:
    explicit X11ErrorGuard(Display* display)
        : m_display(display)
    {
        ensureX11ErrorHandler();
        g_x11ErrorCount = 0;
    }

    X11ErrorGuard(const X11ErrorGuard&) = delete;
    X11ErrorGuard& operator=(const X11ErrorGuard&) = delete;
    ~X11ErrorGuard() = default;

    /// Whether any request in this scope failed, every reply drained first.
    [[nodiscard]] bool failed() const
    {
        XSync(m_display, False);

        return g_x11ErrorCount != 0;
    }

private:
    Display* m_display = nullptr;
};

}  // namespace sidescopes
