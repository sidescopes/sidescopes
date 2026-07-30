// The interactive region border on Linux: an override-redirect X11 window
// ringing the region, drawn with cairo OUTSIDE the region so its strokes never
// enter the scoped pixels, its interior shaped click-through so the editor
// beneath keeps working. The band resizes on its edges and corners, moves from
// the tab above the top edge, toggles attach from the tab's glyph and
// dismisses from the hover close badge - the same affordances the macOS and
// Windows borders carry, over the shared overlay foundation and the portable
// region_geometry zone math.

#include "platform/linux/region_border_x11.h"

#include <X11/cursorfont.h>

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

constexpr double DashLength = 5.0;
constexpr double HandleProtrude = 2.0;

/// The label band's height when a label is worn, zero otherwise. The band
/// rides above the region so the tab clears the top handles.
double labelBandHeight(const BorderX11State& state)
{
    return state.label.empty() ? 0.0 : LabelBand;
}

/// The region's top-left within the border window (window-local pixels).
void interiorOrigin(const BorderX11State& state, double& localX, double& localY)
{
    localX = WindowPad;
    localY = WindowPad + labelBandHeight(state);
}

/// The interactive strips - the band frame around the click-through interior,
/// in window-local pixels. Four rectangles: the top strip carries the label
/// band, the others ring the region.
std::vector<IntRect> bandStrips(const BorderX11State& state)
{
    double interiorX = 0.0;
    double interiorY = 0.0;
    interiorOrigin(state, interiorX, interiorY);
    const int left = static_cast<int>(interiorX);
    const int top = static_cast<int>(interiorY);
    const int right = static_cast<int>(interiorX + state.region.width);
    const int bottom = static_cast<int>(interiorY + state.region.height);
    const int windowWidth = static_cast<int>(state.region.width + 2 * WindowPad);
    const int windowHeight = static_cast<int>(state.region.height + 2 * WindowPad + labelBandHeight(state));

    return {
        IntRect{0, 0, windowWidth, top},                         // above (incl. label band)
        IntRect{0, bottom, windowWidth, windowHeight - bottom},  // below
        IntRect{0, top, left, bottom - top},                     // left
        IntRect{right, top, windowWidth - right, bottom - top},  // right
    };
}

/// The close badge's centre in window-local pixels: off the region's top-right
/// corner, on the band, shown only while the region is wide enough and the
/// band is hovered.
void closeBadgeCentre(const BorderX11State& state, double& cx, double& cy)
{
    double interiorX = 0.0;
    double interiorY = 0.0;
    interiorOrigin(state, interiorX, interiorY);
    cx = interiorX + state.region.width + CloseCornerInset;
    cy = interiorY - CloseCornerInset;
}

bool closeBadgeVisible(const BorderX11State& state)
{
    return state.region.width >= MinimumWidthForClose && state.hoverZone != ZoneNone;
}

void drawRegionRing(cairo_t* canvas, double x, double y, double width, double height, bool attached)
{
    // The measured-edge ring: a warm tint for an attached region, plain
    // otherwise - the tell the region belongs to a window.
    if (attached) {
        cairo_set_source_rgba(canvas, 0.95, 0.72, 0.38, 0.95);
    } else {
        cairo_set_source_rgba(canvas, 0.9, 0.9, 0.92, 0.9);
    }
    cairo_set_line_width(canvas, 1.5);
    const double dashes[] = {DashLength, DashLength};
    cairo_set_dash(canvas, dashes, 2, 0.0);
    cairo_rectangle(canvas, x, y, width, height);
    cairo_stroke(canvas);
    cairo_set_dash(canvas, nullptr, 0, 0.0);
}

void drawHandle(cairo_t* canvas, double x, double y)
{
    cairo_set_source_rgba(canvas, 0.98, 0.98, 0.98, 0.95);
    cairo_arc(canvas, x, y, HandleRadius, 0.0, 2 * M_PI);
    cairo_fill(canvas);
    cairo_set_source_rgba(canvas, 0.1, 0.1, 0.1, 0.6);
    cairo_arc(canvas, x, y, HandleRadius, 0.0, 2 * M_PI);
    cairo_set_line_width(canvas, 1.0);
    cairo_stroke(canvas);
}

void drawHandles(cairo_t* canvas, double x, double y, double width, double height)
{
    const double protrude = HandleProtrude;
    const double xs[] = {x - protrude, x + width / 2, x + width + protrude};
    const double ys[] = {y - protrude, y + height / 2, y + height + protrude};
    for (double hx : xs) {
        for (double hy : ys) {
            if (hx == x + width / 2 && hy == y + height / 2) {
                continue;  // no centre handle
            }
            drawHandle(canvas, hx, hy);
        }
    }
}

void drawLabelTab(cairo_t* canvas, const BorderX11State& state, double interiorX, double interiorY)
{
    if (state.label.empty()) {
        return;
    }
    const double tabY = interiorY - LabelBand;
    cairo_select_font_face(canvas, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(canvas, 12.0);
    cairo_text_extents_t extents;
    cairo_text_extents(canvas, state.label.c_str(), &extents);
    const double tabWidth = TabAttachZone + extents.width + 12.0;
    // The tab's plate.
    cairo_set_source_rgba(canvas, 0.12, 0.12, 0.13, 0.92);
    cairo_rectangle(canvas, interiorX, tabY, std::min(tabWidth, state.region.width + WindowPad), LabelBand - 2.0);
    cairo_fill(canvas);
    // The attach toggle glyph at the tab's left: a filled dot for attached, a
    // struck one for the global region.
    const double glyphX = interiorX + TabAttachZone / 2.0;
    const double glyphY = tabY + LabelBand / 2.0 - 1.0;
    cairo_set_source_rgba(canvas, state.attachedRegion ? 0.95 : 0.6, state.attachedRegion ? 0.72 : 0.6, 0.38, 1.0);
    cairo_arc(canvas, glyphX, glyphY, 3.5, 0.0, 2 * M_PI);
    cairo_fill(canvas);
    if (!state.attachedRegion) {
        cairo_set_source_rgba(canvas, 0.85, 0.85, 0.85, 0.9);
        cairo_set_line_width(canvas, 1.2);
        cairo_move_to(canvas, glyphX - 4.5, glyphY + 4.5);
        cairo_line_to(canvas, glyphX + 4.5, glyphY - 4.5);
        cairo_stroke(canvas);
    }
    // The label text.
    cairo_set_source_rgba(canvas, 0.92, 0.92, 0.92, 1.0);
    cairo_move_to(canvas, interiorX + TabAttachZone, tabY + LabelBand / 2.0 + 4.0);
    cairo_show_text(canvas, state.label.c_str());
}

void drawCloseBadge(cairo_t* canvas, const BorderX11State& state)
{
    if (!closeBadgeVisible(state)) {
        return;
    }
    double cx = 0.0;
    double cy = 0.0;
    closeBadgeCentre(state, cx, cy);
    cairo_set_source_rgba(canvas, 0.85, 0.25, 0.25, 0.95);
    cairo_arc(canvas, cx, cy, CloseRadius, 0.0, 2 * M_PI);
    cairo_fill(canvas);
    cairo_set_source_rgba(canvas, 0.98, 0.98, 0.98, 1.0);
    cairo_set_line_width(canvas, 1.5);
    const double arm = CloseRadius * 0.5;
    cairo_move_to(canvas, cx - arm, cy - arm);
    cairo_line_to(canvas, cx + arm, cy + arm);
    cairo_move_to(canvas, cx + arm, cy - arm);
    cairo_line_to(canvas, cx - arm, cy + arm);
    cairo_stroke(canvas);
}

}  // namespace

BorderX11State& border()
{
    static BorderX11State state;
    return state;
}

bool borderWearsLabel(const BorderX11State& state)
{
    return !state.label.empty();
}

void paintBorder(BorderX11State& state)
{
    cairo_t* canvas = state.window.beginFrame();
    if (canvas == nullptr) {
        return;
    }
    cairo_set_operator(canvas, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(canvas, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(canvas);
    cairo_set_operator(canvas, CAIRO_OPERATOR_OVER);

    double interiorX = 0.0;
    double interiorY = 0.0;
    interiorOrigin(state, interiorX, interiorY);
    drawRegionRing(canvas, interiorX, interiorY, state.region.width, state.region.height, state.attachedRegion);
    drawHandles(canvas, interiorX, interiorY, state.region.width, state.region.height);
    drawLabelTab(canvas, state, interiorX, interiorY);
    drawCloseBadge(canvas, state);
    state.window.endFrame();
}

namespace {

/// The X cursor for a grab zone, from the cursor font, cached per shape.
::Cursor zoneCursor(unsigned zone)
{
    unsigned int shape = XC_left_ptr;
    const bool left = (zone & ZoneLeft) != 0;
    const bool right = (zone & ZoneRight) != 0;
    const bool top = (zone & ZoneTop) != 0;
    const bool bottom = (zone & ZoneBottom) != 0;
    if ((left && top) || (right && bottom)) {
        shape = XC_top_left_corner;
    } else if ((right && top) || (left && bottom)) {
        shape = XC_top_right_corner;
    } else if (left || right) {
        shape = XC_sb_h_double_arrow;
    } else if (top || bottom) {
        shape = XC_sb_v_double_arrow;
    } else if ((zone & ZoneMove) != 0) {
        shape = XC_fleur;
    }

    return XCreateFontCursor(overlayDisplay(), shape);
}

/// The grab zone under a root point: a corner first, then an edge or the move
/// band. Root coordinates throughout, the same space state.region lives in.
unsigned zoneAtRoot(const BorderX11State& state, int rootX, int rootY)
{
    const unsigned corner = cornerZoneAt(state.region, rootX, rootY, 1.0);
    if (corner != ZoneNone) {
        return corner;
    }

    return edgeOrMoveZoneAt(state.region, rootX, rootY, 1.0);
}

/// Whether a root point is on the close badge.
bool onCloseBadge(const BorderX11State& state, int rootX, int rootY)
{
    if (!closeBadgeVisible(state)) {
        return false;
    }
    double cx = 0.0;
    double cy = 0.0;
    closeBadgeCentre(state, cx, cy);
    const double dx = rootX - state.windowX - cx;
    const double dy = rootY - state.windowY - cy;

    return dx * dx + dy * dy <= CloseHitRadius * CloseHitRadius;
}

/// Whether a root point is on the attach toggle glyph in the label tab.
bool onAttachToggle(const BorderX11State& state, int rootX, int rootY)
{
    if (state.label.empty()) {
        return false;
    }
    double interiorX = 0.0;
    double interiorY = 0.0;
    interiorOrigin(state, interiorX, interiorY);
    const double localX = rootX - state.windowX;
    const double localY = rootY - state.windowY;

    return localX >= interiorX && localX < interiorX + TabAttachZone && localY >= interiorY - LabelBand &&
           localY < interiorY;
}

void handleBorderPress(BorderX11State& state, const XButtonEvent& button)
{
    if (button.button != Button1) {
        return;
    }
    if (onCloseBadge(state, button.x_root, button.y_root)) {
        state.dismissed = true;

        return;
    }
    if (onAttachToggle(state, button.x_root, button.y_root)) {
        state.attachToggled = true;

        return;
    }
    state.dragZone = zoneAtRoot(state, button.x_root, button.y_root);
    state.dragStartRegion = state.region;
    state.dragStartRootX = button.x_root;
    state.dragStartRootY = button.y_root;
    state.dragging = true;
    state.editing = true;
}

void handleBorderMotion(BorderX11State& state, const XMotionEvent& motion)
{
    if (state.dragging) {
        const double dx = motion.x_root - state.dragStartRootX;
        const double dy = motion.y_root - state.dragStartRootY;
        state.editedRegion = draggedRegionRect(state.dragZone, state.dragStartRegion, dx, dy, MinimumRegionSize);
        state.edited = true;

        return;
    }
    const unsigned zone = zoneAtRoot(state, motion.x_root, motion.y_root);
    if (zone != state.hoverZone) {
        state.hoverZone = zone;
        XDefineCursor(overlayDisplay(), state.window.handle(), zoneCursor(zone));
        paintBorder(state);
    }
}

void handleBorderRelease(BorderX11State& state, const XButtonEvent& button)
{
    if (button.button != Button1 || !state.dragging) {
        return;
    }
    state.dragging = false;
    state.editing = false;
    const double dx = button.x_root - state.dragStartRootX;
    const double dy = button.y_root - state.dragStartRootY;
    state.editedRegion = draggedRegionRect(state.dragZone, state.dragStartRegion, dx, dy, MinimumRegionSize);
    state.edited = true;
}

void handleBorderEvent(BorderX11State& state, const XEvent& event)
{
    switch (event.type) {
    case Expose:
        if (event.xexpose.count == 0) {
            paintBorder(state);
        }
        break;
    case ButtonPress:
        handleBorderPress(state, event.xbutton);
        break;
    case MotionNotify:
        handleBorderMotion(state, event.xmotion);
        break;
    case ButtonRelease:
        handleBorderRelease(state, event.xbutton);
        break;
    default:
        break;
    }
}

}  // namespace

void showBorder(uint32_t displayId, const DisplayGeometry& geometry, const LocalRect& regionRoot,
                const std::string& label, bool attached)
{
    BorderX11State& state = border();
    state.displayId = displayId;
    state.displayOriginX = geometry.originX;
    state.displayOriginY = geometry.originY;
    state.displayWidth = geometry.widthPoints;
    state.displayHeight = geometry.heightPoints;
    state.region = regionRoot;
    state.label = label;
    state.attachedRegion = attached;

    const int windowWidth = static_cast<int>(regionRoot.width + 2 * WindowPad);
    const int windowHeight = static_cast<int>(regionRoot.height + 2 * WindowPad + labelBandHeight(state));
    state.windowX = static_cast<int>(regionRoot.x - WindowPad);
    state.windowY = static_cast<int>(regionRoot.y - WindowPad - labelBandHeight(state));

    if (!state.window.created()) {
        BorderX11State* raw = &state;
        if (!state.window.create(state.windowX, state.windowY, windowWidth, windowHeight,
                                 [raw](const XEvent& event) { handleBorderEvent(*raw, event); })) {
            return;
        }
    } else {
        state.window.place(state.windowX, state.windowY, windowWidth, windowHeight);
    }
    // Only the band takes the pointer; the region interior is the editor's.
    state.window.setInputRegion(bandStrips(state));
    state.visible = true;
    paintBorder(state);
}

void hideBorder()
{
    BorderX11State& state = border();
    if (state.window.created()) {
        state.window.destroy();
    }
    state.visible = false;
    state.dragging = false;
    state.editing = false;
    state.hoverZone = ZoneNone;
}

}  // namespace sidescopes
