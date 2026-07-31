#include "platform/linux/x11_overlay.h"

#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo-xlib.h>

#include <unordered_map>

namespace sidescopes {
namespace {

/// The live windows, by handle, for event dispatch. Main-thread only.
std::unordered_map<::Window, OverlayWindow::EventHandler>& handlers()
{
    static std::unordered_map<::Window, OverlayWindow::EventHandler> live;
    return live;
}

/// A 32-bit ARGB visual when the server has one, for real transparency;
/// the default visual otherwise.
bool findArgbVisual(Display* display, XVisualInfo& info)
{
    return XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &info) == True;
}

}  // namespace

Display* overlayDisplay()
{
    static Display* display = XOpenDisplay(nullptr);
    return display;
}

OverlayWindow::~OverlayWindow()
{
    destroy();
}

bool OverlayWindow::create(int x, int y, int width, int height, EventHandler handler)
{
    Display* display = overlayDisplay();
    if (display == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    destroy();

    XVisualInfo visual;
    const bool argb = findArgbVisual(display, visual);
    XSetWindowAttributes attributes;
    attributes.override_redirect = True;
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                            KeyReleaseMask | StructureNotifyMask;
    attributes.background_pixel = 0;
    attributes.border_pixel = 0;
    unsigned long valueMask = CWOverrideRedirect | CWEventMask | CWBackPixel | CWBorderPixel;
    Visual* chosenVisual = CopyFromParent;
    int depth = CopyFromParent;
    if (argb) {
        // A 32-bit window needs its own colormap; the default one belongs to
        // the default visual and a mismatch is a BadMatch at create. Kept in
        // m_colormap so destroy() frees it rather than leaking one per cycle.
        m_colormap = XCreateColormap(display, DefaultRootWindow(display), visual.visual, AllocNone);
        attributes.colormap = m_colormap;
        valueMask |= CWColormap;
        chosenVisual = visual.visual;
        depth = visual.depth;
    }
    m_window =
        XCreateWindow(display, DefaultRootWindow(display), x, y, static_cast<unsigned int>(width),
                      static_cast<unsigned int>(height), 0, depth, InputOutput, chosenVisual, valueMask, &attributes);
    if (m_window == 0) {
        return false;
    }
    m_width = width;
    m_height = height;
    m_surface =
        cairo_xlib_surface_create(display, m_window, argb ? visual.visual : DefaultVisual(display, 0), width, height);
    handlers()[m_window] = std::move(handler);
    XMapRaised(display, m_window);
    XFlush(display);

    return true;
}

void OverlayWindow::destroy()
{
    Display* display = overlayDisplay();
    if (m_window == 0 || display == nullptr) {
        return;
    }
    if (m_keyboardGrabbed) {
        XUngrabKeyboard(display, CurrentTime);
        m_keyboardGrabbed = false;
    }
    if (m_context != nullptr) {
        cairo_destroy(m_context);
        m_context = nullptr;
    }
    if (m_surface != nullptr) {
        cairo_surface_destroy(m_surface);
        m_surface = nullptr;
    }
    handlers().erase(m_window);
    XDestroyWindow(display, m_window);
    // Freed after the window that referenced it, and only when create() made
    // one (the ARGB path); the default-visual window carries none.
    if (m_colormap != 0) {
        XFreeColormap(display, m_colormap);
        m_colormap = 0;
    }
    XFlush(display);
    m_window = 0;
}

void OverlayWindow::setClickThrough(bool clickThrough) const
{
    Display* display = overlayDisplay();
    if (m_window == 0 || display == nullptr) {
        return;
    }
    if (clickThrough) {
        XShapeCombineRectangles(display, m_window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
    } else {
        XShapeCombineMask(display, m_window, ShapeInput, 0, 0, None, ShapeSet);
    }
    XFlush(display);
}

namespace {

/// The rectangles as the shape extension takes them, window-local.
std::vector<XRectangle> shapeRectangles(const std::vector<IntRect>& rects)
{
    std::vector<XRectangle> shape;
    shape.reserve(rects.size());
    for (const IntRect& rect : rects) {
        shape.push_back(XRectangle{static_cast<short>(rect.x), static_cast<short>(rect.y),
                                   static_cast<unsigned short>(rect.width), static_cast<unsigned short>(rect.height)});
    }

    return shape;
}

}  // namespace

void OverlayWindow::setInputRegion(const std::vector<IntRect>& rects) const
{
    Display* display = overlayDisplay();
    if (m_window == 0 || display == nullptr) {
        return;
    }
    std::vector<XRectangle> shape = shapeRectangles(rects);
    XShapeCombineRectangles(display, m_window, ShapeInput, 0, 0, shape.data(), static_cast<int>(shape.size()), ShapeSet,
                            Unsorted);
    XFlush(display);
}

void OverlayWindow::setBoundingShape(const std::vector<IntRect>& rects) const
{
    Display* display = overlayDisplay();
    if (m_window == 0 || display == nullptr) {
        return;
    }
    std::vector<XRectangle> shape = shapeRectangles(rects);
    XShapeCombineRectangles(display, m_window, ShapeBounding, 0, 0, shape.data(), static_cast<int>(shape.size()),
                            ShapeSet, Unsorted);
    XFlush(display);
}

void OverlayWindow::grabKeyboard()
{
    Display* display = overlayDisplay();
    if (m_window == 0 || display == nullptr) {
        return;
    }
    m_keyboardGrabbed =
        XGrabKeyboard(display, m_window, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
}

cairo_t* OverlayWindow::beginFrame()
{
    if (m_surface == nullptr) {
        return nullptr;
    }
    if (m_context == nullptr) {
        m_context = cairo_create(m_surface);
    }
    // Paint into an offscreen group, not the window: a repaint that clears
    // and redraws directly on the window hands the compositor blank
    // intermediate states, which a live desktop shows as flicker.
    cairo_push_group_with_content(m_context, CAIRO_CONTENT_COLOR_ALPHA);
    return m_context;
}

void OverlayWindow::endFrame()
{
    if (m_surface == nullptr || m_context == nullptr) {
        return;
    }
    // The finished frame lands on the window as ONE replace operation.
    cairo_pop_group_to_source(m_context);
    cairo_set_operator(m_context, CAIRO_OPERATOR_SOURCE);
    cairo_paint(m_context);
    cairo_set_operator(m_context, CAIRO_OPERATOR_OVER);
    cairo_surface_flush(m_surface);
    XFlush(overlayDisplay());
}

void OverlayWindow::place(int x, int y, int width, int height)
{
    Display* display = overlayDisplay();
    if (m_window == 0 || display == nullptr || width <= 0 || height <= 0) {
        return;
    }
    XMoveResizeWindow(display, m_window, x, y, static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    if (width != m_width || height != m_height) {
        m_width = width;
        m_height = height;
        cairo_xlib_surface_set_size(m_surface, width, height);
    }
    XFlush(display);
}

void pumpOverlayEvents()
{
    Display* display = overlayDisplay();
    if (display == nullptr) {
        return;
    }
    while (XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
        const auto found = handlers().find(event.xany.window);
        if (found != handlers().end() && found->second) {
            found->second(event);
        }
    }
}

}  // namespace sidescopes
