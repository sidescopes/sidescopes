// The picker sheets: one fullscreen dimmed override-redirect window per
// display, dragged on exactly like the macOS and Windows overlays. Painting
// is cairo over the shared overlay foundation; the region math is the same
// portable region_geometry toolkit the other platforms consume, so only the
// drawing and the event plumbing are new here.

#include "platform/linux/region_picker_x11.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cmath>

#include "platform/linux/pin_cursor.h"
#include "platform/linux/x11_pixels.h"

namespace sidescopes {
namespace {

// The overlay dress, matching the other platforms' picker: a dim wash over
// everything, suggestions outlined with a fill on hover, the drag rectangle
// clear of the wash so the pixels being chosen stay legible.
constexpr double DimAlpha = 0.45;
constexpr double HardDimAlpha = 0.72;  // outside an attached draw's clamp
constexpr double SuggestionAlpha = 0.18;
constexpr double BannerMargin = 18.0;
constexpr double BannerPad = 10.0;
constexpr double LabelSize = 13.0;
constexpr double BannerSize = 14.0;

/// The banner's mode line, matching the shortcut vocabulary the other
/// platforms teach: the picker is one tool with four modes.
const char* bannerText(const PickerX11State& picker)
{
    if (picker.pinMode) {
        return "click to pin a color - shift keeps picking - esc done";
    }
    if (picker.drawMode) {
        return "drag to choose a region - A window - F faces - esc cancels";
    }
    if (picker.facesMode) {
        return picker.facesScanned ? "click a face - A window - D draw - esc cancels"
                                   : "scanning for faces - A window - D draw - esc cancels";
    }
    return "click a window or drag inside one - D draw - F faces - esc cancels";
}

void fillRect(cairo_t* canvas, const LocalRect& rect)
{
    cairo_rectangle(canvas, rect.x, rect.y, rect.width, rect.height);
    cairo_fill(canvas);
}

/// The wash with a hole: dim everything except @p clear, in four bands, so
/// the cleared rectangle needs no compositing tricks.
void paintWashAround(cairo_t* canvas, double alpha, int width, int height, const LocalRect& clear)
{
    cairo_set_source_rgba(canvas, 0.0, 0.0, 0.0, alpha);
    fillRect(canvas, {0.0, 0.0, static_cast<double>(width), clear.y});
    fillRect(canvas, {0.0, clear.y, clear.x, clear.height});
    fillRect(canvas, {clear.x + clear.width, clear.y, width - clear.x - clear.width, clear.height});
    fillRect(canvas, {0.0, clear.y + clear.height, static_cast<double>(width), height - clear.y - clear.height});
}

void paintLabel(cairo_t* canvas, double x, double y, double size, const std::string& text)
{
    cairo_select_font_face(canvas, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(canvas, size);
    cairo_text_extents_t extents;
    cairo_text_extents(canvas, text.c_str(), &extents);
    cairo_set_source_rgba(canvas, 0.08, 0.08, 0.08, 0.85);
    cairo_rectangle(canvas, x - BannerPad, y - extents.height - BannerPad, extents.width + BannerPad * 2,
                    extents.height + BannerPad * 2);
    cairo_fill(canvas);
    cairo_set_source_rgba(canvas, 0.92, 0.92, 0.92, 1.0);
    cairo_move_to(canvas, x, y);
    cairo_show_text(canvas, text.c_str());
}

void paintSuggestions(cairo_t* canvas, const PickerX11State& picker)
{
    for (std::size_t index = 0; index < picker.suggestions.size(); ++index) {
        const auto& [rect, label] = picker.suggestions[index];
        const bool hovered = static_cast<int>(index) == picker.hoveredSuggestion;
        if (hovered) {
            cairo_set_source_rgba(canvas, 0.35, 0.55, 0.95, SuggestionAlpha);
            fillRect(canvas, rect);
        }
        cairo_set_source_rgba(canvas, 0.55, 0.7, 0.98, hovered ? 0.95 : 0.55);
        cairo_set_line_width(canvas, hovered ? 2.5 : 1.5);
        cairo_rectangle(canvas, rect.x, rect.y, rect.width, rect.height);
        cairo_stroke(canvas);
        if (hovered && !label.empty()) {
            paintLabel(canvas, rect.x + BannerPad, rect.y + BannerPad + LabelSize, LabelSize, label);
        }
    }
}

void paintDrag(cairo_t* canvas, const PickerX11State& picker)
{
    const LocalRect selection =
        selectionRectFromDrag(picker.dragStartX, picker.dragStartY, picker.dragCurrentX, picker.dragCurrentY);
    cairo_set_source_rgba(canvas, 0.95, 0.95, 0.95, 0.9);
    cairo_set_line_width(canvas, 1.5);
    cairo_rectangle(canvas, selection.x, selection.y, selection.width, selection.height);
    cairo_stroke(canvas);
}

}  // namespace

std::vector<std::unique_ptr<PickerX11State>>& openPickers()
{
    static std::vector<std::unique_ptr<PickerX11State>> pickers;
    return pickers;
}

/// What the sheet will cover, read before it is mapped. Only wanted where
/// no compositing manager can blend the sheet's transparency; there the
/// snapshot stands in for the live screen, which cannot show through.
cairo_surface_t* grabBackdrop(int originX, int originY, int width, int height)
{
    Display* display = overlayDisplay();
    if (display == nullptr) {
        return nullptr;
    }
    XImage* image = XGetImage(display, DefaultRootWindow(display), originX, originY, static_cast<unsigned int>(width),
                              static_cast<unsigned int>(height), AllPlanes, ZPixmap);
    if (image == nullptr) {
        return nullptr;
    }
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        XDestroyImage(image);

        return nullptr;
    }
    unsigned char* out = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = out + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4;
            unpackPixelBgra(image, XGetPixel(image, x, y), pixel[0], pixel[1], pixel[2]);
            pixel[3] = 255;
        }
    }
    cairo_surface_mark_dirty(surface);
    XDestroyImage(image);

    return surface;
}

/// What every sheet starts from: a clear frame, and where the sheet cannot
/// be transparent, the snapshot standing in for the screen behind it.
void paintSheetBase(cairo_t* canvas, const PickerX11State& picker)
{
    // Start from clear so the wash's alpha is exact, not accumulated.
    cairo_set_operator(canvas, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(canvas, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(canvas);
    cairo_set_operator(canvas, CAIRO_OPERATOR_OVER);
    if (picker.backdrop != nullptr) {
        // Stands where the real screen would show through, so every wash
        // below lands on the content rather than on black.
        cairo_set_source_surface(canvas, picker.backdrop, 0.0, 0.0);
        cairo_paint(canvas);
    }
}

void paintPicker(PickerX11State& picker)
{
    cairo_t* canvas = picker.window.beginFrame();
    if (canvas == nullptr) {
        return;
    }
    paintSheetBase(canvas, picker);

    const bool dragActive = picker.dragging;
    const LocalRect selection =
        selectionRectFromDrag(picker.dragStartX, picker.dragStartY, picker.dragCurrentX, picker.dragCurrentY);
    if (picker.pinMode) {
        // Pinning judges color; a wash would mislead. No dim at all.
    } else if (picker.constrained) {
        // The attached draw's dress: hard dim outside the clamp, the normal
        // wash inside it minus the live selection.
        cairo_set_source_rgba(canvas, 0.0, 0.0, 0.0, HardDimAlpha);
        fillRect(canvas, {0.0, 0.0, static_cast<double>(picker.width), static_cast<double>(picker.height)});
        cairo_set_operator(canvas, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(canvas, 0.0, 0.0, 0.0, 0.0);
        fillRect(canvas, picker.constraintRect);
        cairo_set_operator(canvas, CAIRO_OPERATOR_OVER);
        if (dragActive) {
            // Wash the clamp except the selection; the selection stays clear.
            cairo_save(canvas);
            cairo_rectangle(canvas, picker.constraintRect.x, picker.constraintRect.y, picker.constraintRect.width,
                            picker.constraintRect.height);
            cairo_clip(canvas);
            paintWashAround(canvas, DimAlpha, picker.width, picker.height, selection);
            cairo_restore(canvas);
        }
        if (!picker.constraintLabel.empty()) {
            paintLabel(canvas, picker.constraintRect.x + BannerPad, picker.constraintRect.y + BannerPad + LabelSize,
                       LabelSize, picker.constraintLabel);
        }
    } else if (dragActive) {
        paintWashAround(canvas, DimAlpha, picker.width, picker.height, selection);
    } else {
        cairo_set_source_rgba(canvas, 0.0, 0.0, 0.0, DimAlpha);
        fillRect(canvas, {0.0, 0.0, static_cast<double>(picker.width), static_cast<double>(picker.height)});
    }

    if (!picker.pinMode && !picker.drawMode && !dragActive) {
        paintSuggestions(canvas, picker);
    }
    if (dragActive) {
        paintDrag(canvas, picker);
    }
    paintLabel(canvas, BannerMargin + BannerPad, picker.height - BannerMargin, BannerSize, bannerText(picker));
    picker.window.endFrame();
}

namespace {

int suggestionAt(const PickerX11State& picker, int x, int y)
{
    // Frontmost first, so the first hit is the visible one under the point.
    for (std::size_t index = 0; index < picker.suggestions.size(); ++index) {
        const LocalRect& rect = picker.suggestions[index].first;
        if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void handleButtonPress(PickerX11State& picker, const XButtonEvent& button)
{
    if (button.button != Button1) {
        return;
    }
    picker.dragging = true;
    picker.dragStartX = button.x;
    picker.dragStartY = button.y;
    picker.dragCurrentX = button.x;
    picker.dragCurrentY = button.y;
    picker.shiftDown = (button.state & ShiftMask) != 0;
    // A drag beginning over a suggestion in window mode clamps to it: the
    // attached draw. The clamp holds until the release.
    if (!picker.drawMode && !picker.facesMode && !picker.pinMode) {
        const int over = suggestionAt(picker, button.x, button.y);
        if (over >= 0) {
            picker.constrained = true;
            picker.constraintRect = picker.suggestions[over].first;
            picker.constraintLabel = picker.suggestions[over].second;
            picker.pickDragging = true;
        }
    }
    paintPicker(picker);
}

/// Clamps a point into the attached draw's rectangle.
void clampToConstraint(const PickerX11State& picker, int& x, int& y)
{
    if (!picker.constrained) {
        return;
    }
    const LocalRect& rect = picker.constraintRect;
    x = std::clamp(x, static_cast<int>(rect.x), static_cast<int>(rect.x + rect.width));
    y = std::clamp(y, static_cast<int>(rect.y), static_cast<int>(rect.y + rect.height));
}

void handleMotion(PickerX11State& picker, const XMotionEvent& motion)
{
    int x = motion.x;
    int y = motion.y;
    if (picker.dragging) {
        clampToConstraint(picker, x, y);
        picker.dragCurrentX = x;
        picker.dragCurrentY = y;
        paintPicker(picker);

        return;
    }
    const int over = suggestionAt(picker, x, y);
    if (over != picker.hoveredSuggestion) {
        picker.hoveredSuggestion = over;
        paintPicker(picker);
    }
}

/// A release resolves the gesture: a pin (point or swatch), a drawn or
/// attached-drawn rectangle, a clicked suggestion, or nothing.
void handleButtonRelease(PickerX11State& picker, const XButtonEvent& button)
{
    if (button.button != Button1 || !picker.dragging) {
        return;
    }
    picker.dragging = false;
    picker.shiftDown = (button.state & ShiftMask) != 0;
    int x = button.x;
    int y = button.y;
    clampToConstraint(picker, x, y);
    const LocalRect selection = selectionRectFromDrag(picker.dragStartX, picker.dragStartY, x, y);
    // A gesture below the click threshold is a click, not a drag.
    const bool isClick = selection.width < 4.0 && selection.height < 4.0;

    if (picker.pinMode) {
        picker.pinnedIsPoint = isClick;
        picker.pinnedPointX = x;
        picker.pinnedPointY = y;
        picker.pinnedSample = selection;
        picker.pinnedKeepOpen = picker.shiftDown;
        picker.pinnedReady = true;

        return;
    }
    if (isClick) {
        const int over = suggestionAt(picker, x, y);
        if (over >= 0 && (!picker.facesMode || picker.facesScanned)) {
            picker.confirmedRect = picker.suggestions[over].first;
            picker.picked = true;
            picker.finished = true;
        }
        // A click on bare wash chooses nothing and keeps the pick open.
        picker.constrained = false;
        picker.pickDragging = false;
        paintPicker(picker);

        return;
    }
    picker.confirmedRect = selection;
    picker.picked = true;
    picker.finished = true;
}

void handleKey(PickerX11State& picker, const XKeyEvent& key)
{
    const KeySym sym = XLookupKeysym(const_cast<XKeyEvent*>(&key), 0);
    if (sym == XK_Escape) {
        picker.picked = false;
        picker.finished = true;

        return;
    }
    if (sym == XK_a || sym == XK_A) {
        switchPickerMode(0);
    } else if (sym == XK_d || sym == XK_D) {
        switchPickerMode(1);
    } else if (sym == XK_f || sym == XK_F) {
        switchPickerMode(2);
    }
}

void handleEvent(PickerX11State& picker, const XEvent& event)
{
    switch (event.type) {
    case Expose:
        if (event.xexpose.count == 0) {
            paintPicker(picker);
        }
        break;
    case ButtonPress:
        handleButtonPress(picker, event.xbutton);
        break;
    case MotionNotify:
        handleMotion(picker, event.xmotion);
        break;
    case ButtonRelease:
        handleButtonRelease(picker, event.xbutton);
        break;
    case KeyPress:
        handleKey(picker, event.xkey);
        break;
    default:
        break;
    }
}

/// The colour the pin swatch wears, pushed by the application each frame.
std::optional<FloatColor>& chipColorSlot()
{
    static std::optional<FloatColor> color;
    return color;
}

const std::optional<FloatColor>& chipColor()
{
    return chipColorSlot();
}

/// The sheet's cursor for its live mode: the pin crosshair while pinning,
/// the plain arrow otherwise. Pin mode carries no dim, so the crosshair is
/// most of what tells the user the sheet is there at all.
void applyPickerCursor(const PickerX11State& picker)
{
    Display* display = overlayDisplay();
    if (display == nullptr || picker.window.handle() == 0) {
        return;
    }
    if (!picker.pinMode) {
        XUndefineCursor(display, picker.window.handle());

        return;
    }
    const ::Cursor cursor = pinCursor(chipColor());
    if (cursor != None) {
        XDefineCursor(display, picker.window.handle(), cursor);
    }
}

}  // namespace

std::unique_ptr<PickerX11State> createPickerSheet(uint32_t displayId, int originX, int originY, int width, int height,
                                                  bool draw, bool faces, bool pin)
{
    auto picker = std::make_unique<PickerX11State>();
    picker->displayId = displayId;
    picker->originX = originX;
    picker->originY = originY;
    picker->width = width;
    picker->height = height;
    picker->drawMode = draw;
    picker->facesMode = faces;
    picker->pinMode = pin;
    PickerX11State* raw = picker.get();
    if (!compositingManagerPresent()) {
        picker->backdrop = grabBackdrop(originX, originY, width, height);
    }
    const bool created = picker->window.create(originX, originY, width, height,
                                               [raw](const XEvent& event) { handleEvent(*raw, event); });
    if (!created) {
        return nullptr;
    }
    applyPickerCursor(*picker);

    return picker;
}

void switchPickerMode(int mode)
{
    for (auto& picker : openPickers()) {
        picker->drawMode = mode == 1;
        picker->facesMode = mode == 2;
        picker->pinMode = mode == 3;
        applyPickerCursor(*picker);
        picker->constrained = false;
        picker->pickDragging = false;
        picker->dragging = false;
        picker->suggestions = picker->facesMode ? picker->faces : picker->windows;
        if (picker->drawMode || picker->pinMode) {
            picker->suggestions.clear();
        }
        picker->hoveredSuggestion = -1;
        paintPicker(*picker);
    }
}

void cancelAllPickerSheets()
{
    if (!openPickers().empty()) {
        openPickers().front()->picked = false;
        openPickers().front()->finished = true;
    }
}

void setPickerChipColor(const std::optional<FloatColor>& color)
{
    chipColorSlot() = color;
    for (auto& picker : openPickers()) {
        if (picker->pinMode) {
            applyPickerCursor(*picker);
        }
    }
}

}  // namespace sidescopes
