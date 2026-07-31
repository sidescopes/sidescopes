#pragma once

namespace sidescopes {

/// The session-type decision, pure so it is testable without an X server or a
/// Wayland socket: an X11 session is one with a usable X @p xDisplay and no
/// Wayland @p waylandDisplay. Either argument may be null (the variable
/// unset); an empty string counts as unset too, which is how a cleared
/// variable arrives.
[[nodiscard]] bool isX11Session(const char* waylandDisplay, const char* xDisplay);

/// Whether the process is running on a pure X11 session, where the X server
/// composites the real screen and XShm reads it directly - no desktop portal,
/// no PipeWire, no consent dialog, exactly as macOS and Windows capture.
///
/// The deciding signal is WAYLAND_DISPLAY: a Wayland session sets it, and
/// XWayland then also sets DISPLAY, so DISPLAY alone cannot tell the two apart.
/// WAYLAND_DISPLAY unset (or empty) with a usable X DISPLAY is a genuine X11
/// session; a Wayland session - where XShm under XWayland would see only X
/// clients, never the real screen - returns false and the portal path serves.
[[nodiscard]] bool runningOnX11Session();

}  // namespace sidescopes
