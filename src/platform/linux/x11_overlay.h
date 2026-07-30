#pragma once

#include <X11/Xlib.h>
#include <cairo/cairo.h>

#include <cstdint>
#include <functional>

namespace sidescopes {

/// One override-redirect X11 window with a cairo surface over it: the
/// building block of the Linux overlays (the picker's per-display sheet, the
/// border's strips). Override-redirect keeps the window manager out - the
/// window sits exactly where it is put - and under XWayland such windows are
/// composited above native Wayland toplevels, which the picker and border
/// depend on (verified against sway; the popup-class treatment is what the
/// GTK menu already relies on).
///
/// Every overlay shares one Display connection, owned by overlayDisplay() for
/// the process's life; events are pumped by pumpOverlayEvents from the main
/// thread, which dispatches to each window's handler. Single-threaded by
/// design, like the platform seams that use it.
class OverlayWindow
{
public:
    using EventHandler = std::function<void(const XEvent&)>;

    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    /// Creates and maps the window at the given root coordinates, 32-bit
    /// visual where the server offers one so the surface can hold real
    /// transparency (a compositing manager - always present on XWayland -
    /// blends it; without one the window falls back to opaque). False when
    /// no display is reachable.
    [[nodiscard]] bool create(int x, int y, int width, int height, EventHandler handler);

    /// Tears the window down and stops event delivery to it.
    void destroy();

    /// Empties the window's input region so every click passes through to
    /// whatever lies beneath - the border's dress. XWayland maps the shape
    /// to the surface's Wayland input region.
    void setClickThrough(bool clickThrough) const;

    /// Grabs the keyboard onto this window so overlay-wide keys (Esc, the
    /// mode letters) arrive while the pick is up. Released on destroy.
    void grabKeyboard();

    /// Begins a cairo frame over the whole window; endFrame flushes it.
    /// The context draws in window-local pixels.
    [[nodiscard]] cairo_t* beginFrame();
    void endFrame();

    /// Moves and resizes in one step; the surface follows.
    void place(int x, int y, int width, int height);

    [[nodiscard]] bool created() const
    {
        return m_window != 0;
    }

    [[nodiscard]] ::Window handle() const
    {
        return m_window;
    }

    [[nodiscard]] int width() const
    {
        return m_width;
    }

    [[nodiscard]] int height() const
    {
        return m_height;
    }

private:
    ::Window m_window = 0;
    cairo_surface_t* m_surface = nullptr;
    cairo_t* m_context = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_keyboardGrabbed = false;
};

/// The overlays' shared X11 connection, opened at first use; null on a
/// session with no X display (the picker then reports it cannot open).
/// Distinct from the desktop services' connection so event consumption
/// here never races a geometry query there.
[[nodiscard]] Display* overlayDisplay();

/// Dispatches every pending X event to the overlay windows' handlers.
/// Called once per frame from the seam polls, on the main thread.
void pumpOverlayEvents();

}  // namespace sidescopes
