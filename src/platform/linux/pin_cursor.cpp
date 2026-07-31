// The pin cursor on Linux: the same crosshair and swatch the macOS and
// Windows pin cursors carry, drawn with cairo into an Xcursor image. The
// geometry constants are theirs, so a pin looks the same wherever it is made.

#include "platform/linux/pin_cursor.h"

#include <X11/Xcursor/Xcursor.h>
#include <cairo/cairo.h>

#include <cmath>
#include <cstdint>

#include "platform/linux/x11_overlay.h"

namespace sidescopes {
namespace {

// Shared with the macOS and Windows pin cursors, in points.
constexpr double PinCursorHotspot = 12.0;
constexpr double PinCursorArm = 8.0;
constexpr double PinCursorGap = 2.0;
constexpr double PinSwatchOffset = 7.0;
constexpr double PinSwatchSize = 13.0;

/// The four arms, gapped at the centre so the sampled pixel stays visible.
void strokeCrosshair(cairo_t* canvas, double centre, double width)
{
    cairo_set_line_width(canvas, width);
    cairo_move_to(canvas, centre, centre - PinCursorArm);
    cairo_line_to(canvas, centre, centre - PinCursorGap);
    cairo_move_to(canvas, centre, centre + PinCursorGap);
    cairo_line_to(canvas, centre, centre + PinCursorArm);
    cairo_move_to(canvas, centre - PinCursorArm, centre);
    cairo_line_to(canvas, centre - PinCursorGap, centre);
    cairo_move_to(canvas, centre + PinCursorGap, centre);
    cairo_line_to(canvas, centre + PinCursorArm, centre);
    cairo_stroke(canvas);
}

void drawSwatch(cairo_t* canvas, double centre, const FloatColor& color)
{
    const double x = centre + PinSwatchOffset;
    cairo_rectangle(canvas, x, x, PinSwatchSize, PinSwatchSize);
    cairo_set_source_rgb(canvas, color.r / 255.0, color.g / 255.0, color.b / 255.0);
    cairo_fill_preserve(canvas);
    // A dark rim under a light ring, so the swatch reads against content of
    // any tone - the same two-tone rule the crosshair follows.
    cairo_set_source_rgba(canvas, 0.1, 0.1, 0.1, 0.7);
    cairo_set_line_width(canvas, 2.0);
    cairo_stroke_preserve(canvas);
    cairo_set_source_rgba(canvas, 0.97, 0.97, 0.97, 0.95);
    cairo_set_line_width(canvas, 1.0);
    cairo_stroke(canvas);
}

/// Whether two samples round to the same 8-bit colour. The application pushes
/// a colour every frame; rebuilding a cursor for a change no eye can see
/// would hand the server a new cursor at the frame rate.
bool sameRounded(const std::optional<FloatColor>& left, const std::optional<FloatColor>& right)
{
    if (left.has_value() != right.has_value()) {
        return false;
    }
    if (!left.has_value()) {
        return true;
    }
    return std::lround(left->r) == std::lround(right->r) && std::lround(left->g) == std::lround(right->g) &&
           std::lround(left->b) == std::lround(right->b);
}

}  // namespace

::Cursor pinCursor(const std::optional<FloatColor>& color)
{
    static ::Cursor cursor = None;
    static std::optional<FloatColor> built;
    static bool everBuilt = false;
    Display* display = overlayDisplay();
    if (display == nullptr) {
        return None;
    }
    if (everBuilt && sameRounded(built, color)) {
        return cursor;
    }

    const int side = static_cast<int>(PinCursorHotspot + PinSwatchOffset + PinSwatchSize) + 4;
    XcursorImage* image = XcursorImageCreate(side, side);
    if (image == nullptr) {
        return cursor;
    }
    image->xhot = static_cast<XcursorDim>(PinCursorHotspot);
    image->yhot = static_cast<XcursorDim>(PinCursorHotspot);

    // XcursorPixel is premultiplied ARGB32, which is cairo's ARGB32 exactly,
    // so cairo draws straight into the image's own pixels.
    cairo_surface_t* surface = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(image->pixels),
                                                                   CAIRO_FORMAT_ARGB32, side, side, side * 4);
    cairo_t* canvas = cairo_create(surface);
    cairo_set_operator(canvas, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(canvas, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(canvas);
    cairo_set_operator(canvas, CAIRO_OPERATOR_OVER);

    const double centre = PinCursorHotspot;
    cairo_set_source_rgba(canvas, 0.1, 0.1, 0.1, 0.85);
    strokeCrosshair(canvas, centre, 3.2);
    cairo_set_source_rgba(canvas, 0.8, 0.8, 0.8, 0.95);
    strokeCrosshair(canvas, centre, 1.5);
    if (color) {
        drawSwatch(canvas, centre, *color);
    }
    cairo_destroy(canvas);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);

    const ::Cursor replacement = XcursorImageLoadCursor(display, image);
    XcursorImageDestroy(image);
    if (replacement == None) {
        return cursor;
    }
    // Freed only after the new one exists, so a sheet wearing the old cursor
    // is never left pointing at a freed resource.
    if (cursor != None) {
        XFreeCursor(display, cursor);
    }
    cursor = replacement;
    built = color;
    everBuilt = true;

    return cursor;
}

}  // namespace sidescopes
