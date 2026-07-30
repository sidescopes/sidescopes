#pragma once

#include <X11/Xlib.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "platform/desktop.h"
#include "platform/focus_resolution.h"
#include "platform/linux/x11_displays.h"

namespace sidescopes {

/// The window services of the Linux desktop seam, spoken in X11 and EWMH:
/// enumeration, geometry, the active window, activation and motion. They see
/// X11 CLIENTS - under XWayland every application the compositor runs through
/// Xwayland, and no native Wayland toplevel, which is the portal's window
/// source to answer for instead.
///
/// Every entry point takes the caller's connection rather than owning one, so
/// the seam keeps a single connection for the process; each guards its own
/// requests, since a window can die between being listed and being asked
/// about and X11's default answer to that is to exit the process.

/// The ordinary application windows on @p displayId, frontmost first, in
/// global desktop points with a top-left origin. Windows owned by @p ownPid
/// are excluded, as are minimized ones, non-application types and anything
/// too small to attach a region to. @p displays is the connected set, which
/// decides - by largest overlap - which display a window belongs to.
[[nodiscard]] std::vector<DesktopWindow> x11OnScreenWindows(Display* display, const std::vector<LinuxDisplay>& displays,
                                                            uint32_t displayId, int64_t ownPid);

/// The window's current rectangle, minimized state and title, or nothing
/// when it no longer exists - which the caller reads as the window having
/// closed.
[[nodiscard]] std::optional<WindowGeometry> x11WindowGeometry(Display* display, uint64_t identity);

/// Every managed window that is on screen, front to back, for the shared
/// focus rule. Unfiltered otherwise - this application's own windows
/// included - because the rule reasons about the whole stacking order.
[[nodiscard]] std::vector<OrderedWindow> x11OrderedWindows(Display* display);

/// The process id behind _NET_ACTIVE_WINDOW, or zero when no window is
/// active or the manager publishes none.
[[nodiscard]] int64_t x11ForegroundApplicationPid(Display* display);

/// Asks the window manager to activate the window - EWMH's request, since
/// the manager owns the stacking order and undoes a bare raise.
void x11ActivateWindow(Display* display, uint64_t identity);

/// Watches the window for geometry changes, invoking @p callback with
/// WindowMotionSignal::Moved as each arrives. Replaces any previous watch.
/// @p display is the caller's connection, used only to end the watch; the
/// watch itself runs on a connection and thread of its own, and the callback
/// arrives on that thread - see the definition for why that is safe here.
/// Only Moved is ever delivered: whether a human's button is down on a
/// FOREIGN window is not observable without a pointer grab, and this
/// instrument does not grab.
void x11WatchWindowMotion(Display* display, uint64_t identity, std::function<void(WindowMotionSignal)> callback);

/// Stops the watch and drops the callback, the watcher thread joined before
/// this returns. Safe when nothing is watched.
void x11UnwatchWindowMotion(Display* display);

}  // namespace sidescopes
