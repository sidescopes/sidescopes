#include "web/region_editor.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "web/band_geometry.h"

namespace sidescopes {
namespace {

// src/platform/macos/region_border_view.h's own numbers. The desktop border
// is a window on the desktop and cannot run here, but its geometry can, and
// a lab whose border is a near-miss of the application's teaches the wrong
// thing about the application.
constexpr float BorderPad = 12.0f;     // the grab band outside the region
constexpr float HandleRadius = 3.5f;   // one handle dot
constexpr float EdgeRing = 1.0f;       // the measured edge's thickness
constexpr int MinimumRegionSize = 24;  // per side, in image pixels

// The close badge, again the desktop's numbers.
constexpr float CrossThickness = 1.3f;
constexpr float CloseRadius = 6.5f;
constexpr float CloseHitRadius = 11.0f;
constexpr float CloseCornerInset = 2.0f;
constexpr float MinimumWidthForClose = 48.0f;
// Dear ImGui picks a circle's segment count from its radius in ITS units and
// knows nothing of the device scale, so a 6.5-point disc is tessellated for
// 6.5 pixels and drawn into thirteen - a visible polygon where AppKit strokes
// a true oval. Pinned high enough that neither the badge nor a handle shows a
// facet at any scale a browser hands us; it costs a few vertices.
constexpr int CircleSegments = 48;

// The border's palette, as the desktop mixes it: neutral greys only, both
// region kinds. Any hue this close to the sampled pixels would skew the
// eye's read of the photograph.
[[nodiscard]] ImU32 grey(float white, float alpha)
{
    const int value = static_cast<int>(std::lround(white * 255.0f));

    return IM_COL32(value, value, value, static_cast<int>(std::lround(alpha * 255.0f)));
}

[[nodiscard]] int clampInt(int value, int low, int high)
{
    return std::max(low, std::min(value, high));
}

}  // namespace

void RegionEditor::reset(int imageWidth, int imageHeight, float fraction)
{
    const int width = std::max(MinimumRegionSize, static_cast<int>(static_cast<float>(imageWidth) * fraction));
    const int height = std::max(MinimumRegionSize, static_cast<int>(static_cast<float>(imageHeight) * fraction));
    m_rect = SsRect{(imageWidth - width) / 2, (imageHeight - height) / 2, width, height};
    m_grab = ZoneNone;
}

void RegionEditor::holdOnScreen(const Placement& placement)
{
    if (!hasRegion() || placement.scale <= 0.0f) {
        return;
    }
    const float left = placement.origin.x + static_cast<float>(m_rect.x) * placement.scale;
    const float top = placement.origin.y + static_cast<float>(m_rect.y) * placement.scale;

    m_heldOnScreen = ImVec4{left, top, static_cast<float>(m_rect.width) * placement.scale,
                            static_cast<float>(m_rect.height) * placement.scale};
}

/// Puts a held region back where it was on screen, in the new picture's own
/// pixels. Clamped into the picture, because a region has to be somewhere the
/// scopes can read: a rectangle left hanging over the letterbox would measure
/// nothing.
void RegionEditor::restoreHeldPosition(const Placement& placement, int imageWidth, int imageHeight)
{
    const ImVec4 held = *m_heldOnScreen;
    m_heldOnScreen.reset();
    if (placement.scale <= 0.0f) {
        return;
    }
    const LocalRect wanted{(held.x - placement.origin.x) / placement.scale,
                           (held.y - placement.origin.y) / placement.scale, held.z / placement.scale,
                           held.w / placement.scale};
    const LocalRect fitted = rectClampedWithin(wanted, imageWidth, imageHeight);
    m_rect = SsRect{static_cast<int>(std::lround(fitted.x)), static_cast<int>(std::lround(fitted.y)),
                    std::max(MinimumRegionSize, static_cast<int>(std::lround(fitted.width))),
                    std::max(MinimumRegionSize, static_cast<int>(std::lround(fitted.height)))};
}

void RegionEditor::clear()
{
    m_heldOnScreen.reset();
    m_rect = SsRect{0, 0, 0, 0};
    m_grab = ZoneNone;
    m_arming = false;
    m_drawing = false;
}

void RegionEditor::armDraw()
{
    // The old region goes at once. The desktop picker dims the screen and
    // takes the border down with it, so leaving the previous rectangle on
    // screen while a new one is being drawn would be a rectangle nothing is
    // measuring.
    m_rect = SsRect{0, 0, 0, 0};
    m_arming = true;
    m_drawing = false;
    m_grab = ZoneNone;
}

RegionEditor::Handles RegionEditor::handlesFor(const ImVec2& topLeft, const ImVec2& bottomRight)
{
    const float midX = (topLeft.x + bottomRight.x) * 0.5f;
    const float midY = (topLeft.y + bottomRight.y) * 0.5f;

    // Corners and edge midpoints, centred on the measurement line, the way
    // the macOS screenshot selection wears them. These are the DOTS ONLY -
    // what a press on one does is platform/region_geometry's answer, not a
    // second opinion kept alongside them.
    return Handles{
        {
            ImVec2{topLeft.x, topLeft.y},
            ImVec2{midX, topLeft.y},
            ImVec2{bottomRight.x, topLeft.y},
            ImVec2{topLeft.x, midY},
            ImVec2{bottomRight.x, midY},
            ImVec2{topLeft.x, bottomRight.y},
            ImVec2{midX, bottomRight.y},
            ImVec2{bottomRight.x, bottomRight.y},
        },
    };
}

unsigned RegionEditor::grabAt(const ImVec2& point, const Placement& placement) const
{
    const auto [topLeft, bottomRight] = screenRect(placement);

    // platform/region_geometry's answer, which is what both desktop borders
    // resolve through. Reimplementing it here with fixed-size boxes around
    // the handle dots looked equivalent and was not: the shared rule caps a
    // corner's reach at a SIXTH of the side, so a small region keeps a move
    // band, and face-sized regions are this product's bread and butter.
    const LocalRect region{topLeft.x, topLeft.y, bottomRight.x - topLeft.x, bottomRight.y - topLeft.y};

    return zoneAtPoint(region, point.x, point.y, BorderPad);
}

void RegionEditor::applyDrag(const ImVec2& delta, int imageWidth, int imageHeight)
{
    // The drag itself is platform/region_geometry's, so a resize behaves the
    // same here as under either desktop border - including how an edge is
    // stopped from crossing its opposite. Only the clamp to the PICTURE is
    // added: the desktop clamps to a display, and the picture is what stands
    // in for one.
    const LocalRect start{static_cast<double>(m_pressed.x), static_cast<double>(m_pressed.y),
                          static_cast<double>(m_pressed.width), static_cast<double>(m_pressed.height)};
    // The delta arrives already in image pixels: the caller divides by the
    // placement's scale, so the region math is in the same units as m_rect.
    const LocalRect dragged = draggedRegionRect(m_grab, start, delta.x, delta.y, MinimumRegionSize);
    const LocalRect fitted = rectClampedWithin(dragged, imageWidth, imageHeight);
    m_rect = SsRect{static_cast<int>(std::lround(fitted.x)), static_cast<int>(std::lround(fitted.y)),
                    static_cast<int>(std::lround(fitted.width)), static_cast<int>(std::lround(fitted.height))};
}

namespace {

/// Holds Dear ImGui's anti-aliasing fringe to ONE DEVICE PIXEL for as long as
/// it is alive, and puts it back after.
///
/// The fringe is measured in Dear ImGui's own units, so on a 2x canvas a
/// one-point stroke is spread across two device pixels and lands washed out -
/// which is why the handle rims and the close badge's ring read fainter here
/// than on the desktop, where AppKit strokes one point into two crisp pixels
/// at full strength.
///
/// Scoped to the region border ON PURPOSE rather than set once for the whole
/// interface: every other pixel of this lab is drawn by the same Dear ImGui
/// code the desktop application draws with and matches it already. The border
/// is the one part the desktop draws with AppKit instead, so it is the one
/// part with a native rendering to be held against.
class DeviceFringe
{
public:
    explicit DeviceFringe(ImDrawList* draw)
        : m_draw(draw),
          m_previous(draw->_FringeScale)
    {
        const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
        if (scale > 0.0f) {
            m_draw->_FringeScale = 1.0f / scale;
        }
    }

    DeviceFringe(const DeviceFringe&) = delete;
    DeviceFringe& operator=(const DeviceFringe&) = delete;

    ~DeviceFringe()
    {
        m_draw->_FringeScale = m_previous;
    }

private:
    ImDrawList* m_draw;
    float m_previous;
};

/// The band outside the region, with the hazard stripes ruled across it.
/// Four clipped rectangles, because a ring is not a clip shape.
void drawBand(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight)
{
    const ImVec2 outer{bottomRight.x + BorderPad, bottomRight.y + BorderPad};
    const ImVec2 outerTopLeft{topLeft.x - BorderPad, topLeft.y - BorderPad};
    // The stripes stop short of the measured-edge ring: crossing it would
    // read as the band bleeding into the measured region.
    const ImVec2 holeTopLeft{topLeft.x - EdgeRing, topLeft.y - EdgeRing};
    const ImVec2 holeBottomRight{bottomRight.x + EdgeRing, bottomRight.y + EdgeRing};
    // The quarters, and their whole-point edges, come from a unit that is
    // tested: adjacent quarters sharing an edge, no overlap, and covering the
    // ring exactly are properties this border has broken twice.
    const std::array<BandRect, 4> bands =
        bandQuarters(BandRect{outerTopLeft.x, outerTopLeft.y, outer.x, outer.y},
                     BandRect{holeTopLeft.x, holeTopLeft.y, holeBottomRight.x, holeBottomRight.y});
    const float height = outer.y - outerTopLeft.y;
    for (const BandRect& band : bands) {
        if (band.empty()) {
            continue;
        }
        draw->PushClipRect(ImVec2{band.left, band.top}, ImVec2{band.right, band.bottom}, true);
        // The WHOLE band, every time, and the scissor decides what survives.
        //
        // Filling each quarter with its own rectangle put a seam across the
        // band at the region's top and bottom edges. These are TRANSLUCENT
        // fills and the canvas is multisampled, so where two of them met on a
        // fractional pixel row both covered it partly and it was blended
        // twice - a horizontal line, exactly where two quarters abut.
        //
        // A scissor is a hard cut with no coverage to share, so four
        // identical full-band rectangles tile perfectly where four abutting
        // ones could not. It is also what the desktop does: one fill, through
        // one ring-shaped clip.
        draw->AddRectFilled(outerTopLeft, outer, grey(0.1f, 0.45f));
        // An integer step keeps the diagonal spacing exact, as the desktop's
        // own loop does; a float counter accumulates rounding across a band.
        const float start = outerTopLeft.x - height;
        // Each stripe runs past both ends of the band, and that overshoot is
        // the whole fix for the rounded ends: Dear ImGui feathers a thick
        // line's ends through its anti-aliasing texture, and the ruling
        // starts a band-height to the left, so the far end of the first
        // stripes and the near end of the last used to land INSIDE the
        // visible band. Pushed outside, the scissor cuts a long edge square
        // and the cap is never drawn where it can be seen.
        //
        // The stripe stays a LINE. Drawn as a filled quad instead - which is
        // what a butt-capped stroke is - it comes out visibly thinner, because
        // Dear ImGui anti-aliases a fill by insetting the shape and adding a
        // fringe, so four points of quad render as about two of solid ink,
        // where the line's textured path lays down all four.
        constexpr float Overshoot = 8.0f;
        for (int step = 0; start + static_cast<float>(step) * 10.0f < outer.x; ++step) {
            const float x = start + static_cast<float>(step) * 10.0f;
            draw->AddLine(ImVec2{x - Overshoot, outer.y + Overshoot},
                          ImVec2{x + height + Overshoot, outerTopLeft.y - Overshoot}, grey(0.9f, 0.45f), 4.0f);
        }
        draw->PopClipRect();
    }
}

/// The measured edge: a dark ring from the region outwards, with white
/// dashes riding over it, so one tone survives any background. The
/// screenshot tools' marching ants, standing still.
void drawMeasuredEdge(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight)
{
    // A FILLED ring, exactly as the desktop fills one: the band from the
    // region out to EdgeRing, with no stroke geometry of its own to disagree
    // with it.
    //
    // It was a stroked AddRect before, and that is not the same shape. A
    // stroke is centred on its path AND Dear ImGui insets the path half a
    // pixel, so a two-wide stroke around the ring's outer edge reached a pixel
    // beyond the ring on the top and left and fell short on the bottom and
    // right - the doubled dark line, on two sides only, that gave it away.
    const ImU32 ringInk = grey(0.1f, 0.85f);
    const ImVec2 outerTopLeft{topLeft.x - EdgeRing, topLeft.y - EdgeRing};
    const ImVec2 outerBottomRight{bottomRight.x + EdgeRing, bottomRight.y + EdgeRing};
    draw->AddRectFilled(outerTopLeft, ImVec2{outerBottomRight.x, topLeft.y}, ringInk);
    draw->AddRectFilled(ImVec2{outerTopLeft.x, bottomRight.y}, outerBottomRight, ringInk);
    draw->AddRectFilled(ImVec2{outerTopLeft.x, topLeft.y}, ImVec2{topLeft.x, bottomRight.y}, ringInk);
    draw->AddRectFilled(ImVec2{bottomRight.x, topLeft.y}, ImVec2{outerBottomRight.x, bottomRight.y}, ringInk);

    // The dashes ride the MIDDLE of that ring, which is where the desktop puts
    // them: it strokes a rectangle inset by half the ring with a pen exactly
    // the ring wide. Filled rectangles rather than lines, because AddLine
    // shifts both endpoints half a pixel to land a hairline on a pixel centre
    // - the same offset already corrected in the close badge's cross - and
    // half a pixel is half this ring.
    constexpr float Dash = 4.0f;
    const ImU32 dashInk = grey(0.97f, 0.95f);
    for (float x = topLeft.x; x < bottomRight.x; x += Dash * 2.0f) {
        const float to = std::min(x + Dash, bottomRight.x);
        draw->AddRectFilled(ImVec2{x, outerTopLeft.y}, ImVec2{to, topLeft.y}, dashInk);
        draw->AddRectFilled(ImVec2{x, bottomRight.y}, ImVec2{to, outerBottomRight.y}, dashInk);
    }
    for (float y = topLeft.y; y < bottomRight.y; y += Dash * 2.0f) {
        const float to = std::min(y + Dash, bottomRight.y);
        draw->AddRectFilled(ImVec2{outerTopLeft.x, y}, ImVec2{topLeft.x, to}, dashInk);
        draw->AddRectFilled(ImVec2{bottomRight.x, y}, ImVec2{outerBottomRight.x, to}, dashInk);
    }
}

}  // namespace

bool RegionEditor::takeDismissed()
{
    const bool dismissed = m_dismissed;
    m_dismissed = false;

    return dismissed;
}

std::pair<ImVec2, ImVec2> RegionEditor::screenRect(const Placement& placement) const
{
    const ImVec2 topLeft{std::round(placement.origin.x + static_cast<float>(m_rect.x) * placement.scale),
                         std::round(placement.origin.y + static_cast<float>(m_rect.y) * placement.scale)};

    return {topLeft, ImVec2{std::round(topLeft.x + static_cast<float>(m_rect.width) * placement.scale),
                            std::round(topLeft.y + static_cast<float>(m_rect.height) * placement.scale)}};
}

ImVec2 RegionEditor::closeCentre(const ImVec2& topLeft, const ImVec2& bottomRight)
{
    // The band's outer top corner, pulled in a touch so the disc mostly
    // rides the band - the desktop's own placement, mirrored for a
    // downward y.
    // Mirrored for a downward y, and EdgeRing mirrors WITH it: on the desktop
    // the badge sits BorderPad - CloseCornerInset + EdgeRing above the
    // region's top, and adding EdgeRing here instead of subtracting it put the
    // badge two points low - which on a thirteen-point disc against a
    // twelve-point band is the difference between riding the band and sitting
    // off its corner.
    return ImVec2{bottomRight.x + BorderPad - CloseCornerInset, topLeft.y - BorderPad + CloseCornerInset - EdgeRing};
}

bool RegionEditor::closeOffered(const Placement& placement) const
{
    // A narrow region keeps its corner for resizing; the badge would sit on
    // top of the grab zones and win a press meant for them.
    return static_cast<float>(m_rect.width) * placement.scale >= MinimumWidthForClose;
}

/// Always visible while the border is up, which is the desktop's rule, and
/// the reason behind it is the part worth keeping: revealing it on hover
/// flickered on every crossing of the band, and crossing the band is what a
/// pointer does all day. It still stands down mid-drag and on a region too
/// narrow to hold it.
bool RegionEditor::closeVisible(const Placement& placement) const
{
    return m_grab == ZoneNone && !m_arming && closeOffered(placement);
}

void RegionEditor::drawCloseBadge(const ImVec2& centre) const
{
    // A DARK disc where the handles are light, so it reads as an action
    // rather than a grip, with the same bright ring and an x.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(centre, CloseRadius, grey(0.1f, 0.85f), CircleSegments);
    draw->AddCircle(centre, CloseRadius, grey(0.97f, 0.95f), CircleSegments, 1.0f);

    // The cross goes through the PATH api rather than AddLine, which offsets
    // both its endpoints by half a pixel to make axis-aligned hairlines land
    // on a pixel centre. AddCircle does no such thing, so the two disagree:
    // measured on a 2x display, the cross sat a whole device pixel down and
    // right of the disc it is centred in. On a badge thirteen points across
    // the uneven dark margin is the first thing the eye finds. Nothing here
    // is axis-aligned, so the offset buys no crispness to trade away.
    const float arm = CloseRadius - 3.7f;
    const ImU32 ink = grey(0.97f, 0.95f);
    const ImVec2 topLeftTip{centre.x - arm, centre.y - arm};
    const ImVec2 bottomRightTip{centre.x + arm, centre.y + arm};
    const ImVec2 bottomLeftTip{centre.x - arm, centre.y + arm};
    const ImVec2 topRightTip{centre.x + arm, centre.y - arm};
    const ImVec2 tip[4] = {topLeftTip, bottomRightTip, bottomLeftTip, topRightTip};

    draw->PathLineTo(topLeftTip);
    draw->PathLineTo(bottomRightTip);
    draw->PathStroke(ink, 0, CrossThickness);
    draw->PathLineTo(bottomLeftTip);
    draw->PathLineTo(topRightTip);
    draw->PathStroke(ink, 0, CrossThickness);
    // The desktop strokes this with ROUND caps and Dear ImGui offers no cap
    // style, so the four ends are capped by hand. Without them the diagonals
    // stop in square corners, which at this size read as burrs and make the
    // arms look unequal.
    for (const ImVec2& end : tip) {
        draw->AddCircleFilled(end, CrossThickness * 0.5f, ink, CircleSegments);
    }
}

void RegionEditor::drawBorder(const Placement& placement, int imageWidth, int imageHeight) const
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const DeviceFringe crispWhileDrawing{draw};
    const auto [topLeft, bottomRight] = screenRect(placement);

    // Bounded by the PICTURE, which is what stands in for a display here, so
    // a region pushed into a corner is cut the same way on both axes.
    //
    // Without this the band simply drew wherever it landed, and how much of
    // it survived depended on how much room happened to be outside the
    // picture on that side: over a letterboxed edge the whole 12 points
    // showed, against a snug one the pane clipped it to a sliver. Two edges
    // of one rectangle stopping differently is what that looked like.
    //
    // The desktop needs no such clip because its band IS a window on the
    // desktop and the display's own edge cuts it - which is the behaviour
    // this reproduces rather than invents.
    draw->PushClipRect(placement.origin,
                       ImVec2{placement.origin.x + static_cast<float>(imageWidth) * placement.scale,
                              placement.origin.y + static_cast<float>(imageHeight) * placement.scale},
                       true);

    drawBand(draw, topLeft, bottomRight);
    drawMeasuredEdge(draw, topLeft, bottomRight);

    // Eight dots: a light disc, a dark rim beneath a near-white ring, so one
    // reads on a bright sky and on a dark shadow alike.
    const Handles handles = handlesFor(topLeft, bottomRight);
    for (const ImVec2& point : handles.point) {
        draw->AddCircleFilled(point, HandleRadius, grey(0.78f, 1.0f), CircleSegments);
        draw->AddCircle(point, HandleRadius, grey(0.1f, 0.7f), CircleSegments, 2.0f);
        draw->AddCircle(point, HandleRadius, grey(0.97f, 0.95f), CircleSegments, 1.0f);
    }

    // Last, as the desktop draws it last: the badge sits ON the band and a
    // band painted afterwards would bury it.
    if (closeVisible(placement)) {
        drawCloseBadge(closeCentre(topLeft, bottomRight));
    }

    draw->PopClipRect();
}

/// A rectangle in the live drag's own language: a solid dark line under a
/// dashed light one, so one tone survives pure white and pure black alike.
/// Dear ImGui has no dashed stroke, so the dashes are walked by hand at the
/// desktop's own 4-on-4-off.
void RegionEditor::dashedRect(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight)
{
    // Both tones go through the PATH api, from the SAME four corners. Through
    // the convenience calls they would not coincide: AddRect insets its
    // rectangle by half a pixel on every side while AddLine shifts its
    // endpoints by half a pixel, so the light dashes would ride a pixel off
    // the dark line they are meant to sit on.
    draw->PathLineTo(topLeft);
    draw->PathLineTo(ImVec2{bottomRight.x, topLeft.y});
    draw->PathLineTo(bottomRight);
    draw->PathLineTo(ImVec2{topLeft.x, bottomRight.y});
    draw->PathStroke(grey(0.1f, 0.85f), ImDrawFlags_Closed, 1.0f);

    const float dash = 4.0f;
    const ImVec2 corner[5] = {topLeft, ImVec2{bottomRight.x, topLeft.y}, bottomRight, ImVec2{topLeft.x, bottomRight.y},
                              topLeft};
    for (int edge = 0; edge < 4; ++edge) {
        const ImVec2 from = corner[edge];
        const ImVec2 to = corner[edge + 1];
        const float span = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
        const float stepX = span > 0.0f ? (to.x - from.x) / span : 0.0f;
        const float stepY = span > 0.0f ? (to.y - from.y) / span : 0.0f;
        for (float at = 0.0f; at < span; at += dash * 2.0f) {
            const float end = std::min(at + dash, span);
            draw->AddLine(ImVec2{from.x + stepX * at, from.y + stepY * at},
                          ImVec2{from.x + stepX * end, from.y + stepY * end}, grey(0.97f, 0.95f), 1.0f);
        }
    }
}

/// What the desktop picker puts on screen while it is open: the workspace
/// dimmed so the selection reads against it, and a banner saying what to do.
/// A page cannot dim a desktop, but it can dim the picture standing in for
/// one, and the instruction is the same.
///
/// While the drag is live the selection is PUNCHED out of the dim - you
/// cannot judge content through a wash - and carries only a dashed
/// rectangle. The hazard band, the handles and the badge belong to a settled
/// region and arrive when the button comes up, which is the desktop's
/// division too: the picker overlay draws the drag, the border view draws
/// the result.
void RegionEditor::drawPickerOverlay(const Placement& placement, int imageWidth, int imageHeight) const
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 topLeft = placement.origin;
    const ImVec2 bottomRight{topLeft.x + static_cast<float>(imageWidth) * placement.scale,
                             topLeft.y + static_cast<float>(imageHeight) * placement.scale};
    const ImU32 dim = grey(0.0f, 0.35f);
    const bool live = m_drawing && hasRegion();

    if (live) {
        const ImVec2 selTopLeft{placement.origin.x + static_cast<float>(m_rect.x) * placement.scale,
                                placement.origin.y + static_cast<float>(m_rect.y) * placement.scale};
        const ImVec2 selBottomRight{selTopLeft.x + static_cast<float>(m_rect.width) * placement.scale,
                                    selTopLeft.y + static_cast<float>(m_rect.height) * placement.scale};
        // Four bands around the hole rather than one wash over everything.
        draw->AddRectFilled(topLeft, ImVec2{bottomRight.x, selTopLeft.y}, dim);
        draw->AddRectFilled(ImVec2{topLeft.x, selBottomRight.y}, bottomRight, dim);
        draw->AddRectFilled(ImVec2{topLeft.x, selTopLeft.y}, ImVec2{selTopLeft.x, selBottomRight.y}, dim);
        draw->AddRectFilled(ImVec2{selBottomRight.x, selTopLeft.y}, ImVec2{bottomRight.x, selBottomRight.y}, dim);
        dashedRect(draw, selTopLeft, selBottomRight);

        return;
    }

    draw->AddRectFilled(topLeft, bottomRight, dim);

    // The banner, centred over the dimmed picture, in the picker's own
    // words: it suggests only what it can actually do. It stands down once
    // the drag begins, as the desktop's does - the selection is the subject
    // then, and a caption over it is in the way.
    const char* line = "Drag to draw a region";
    const ImVec2 text = ImGui::CalcTextSize(line);
    const ImVec2 centre{(topLeft.x + bottomRight.x) * 0.5f, topLeft.y + (bottomRight.y - topLeft.y) * 0.12f};
    const ImVec2 pad{14.0f, 8.0f};
    draw->AddRectFilled(ImVec2{centre.x - text.x * 0.5f - pad.x, centre.y - text.y * 0.5f - pad.y},
                        ImVec2{centre.x + text.x * 0.5f + pad.x, centre.y + text.y * 0.5f + pad.y}, grey(0.08f, 0.88f),
                        6.0f);
    draw->AddText(ImVec2{centre.x - text.x * 0.5f, centre.y - text.y * 0.5f}, grey(0.95f, 1.0f), line);
}

bool RegionEditor::updateDrawing(const Placement& placement, int imageWidth, int imageHeight)
{
    const ImVec2 mouse = ImGui::GetMousePos();
    const float scale = placement.scale > 0.0f ? placement.scale : 1.0f;
    bool changed = false;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
        m_drawing = true;
        m_drawFrom = mouse;
    }
    if (m_drawing && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 from{std::min(m_drawFrom.x, mouse.x), std::min(m_drawFrom.y, mouse.y)};
        const ImVec2 to{std::max(m_drawFrom.x, mouse.x), std::max(m_drawFrom.y, mouse.y)};
        const int left = clampInt(static_cast<int>((from.x - placement.origin.x) / scale), 0, imageWidth);
        const int top = clampInt(static_cast<int>((from.y - placement.origin.y) / scale), 0, imageHeight);
        const int right = clampInt(static_cast<int>((to.x - placement.origin.x) / scale), left, imageWidth);
        const int bottom = clampInt(static_cast<int>((to.y - placement.origin.y) / scale), top, imageHeight);
        m_rect = SsRect{left, top, right - left, bottom - top};
        changed = true;
    }
    if (m_drawing && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_drawing = false;
        m_arming = false;
        // A click rather than a drag lays down nothing, which is the
        // desktop's answer too: a region is a rectangle you meant.
        if (m_rect.width < MinimumRegionSize || m_rect.height < MinimumRegionSize) {
            m_rect = SsRect{0, 0, 0, 0};
        }
        changed = true;
    }

    return changed;
}

void RegionEditor::announceCursor(const Placement& placement) const
{
    const int over = m_grab != ZoneNone ? m_grab : grabAt(ImGui::GetMousePos(), placement);
    if (over == ZoneMove) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (over != ZoneNone) {
        const bool horizontal = (over & (ZoneLeft | ZoneRight)) != 0;
        const bool vertical = (over & (ZoneTop | ZoneBottom)) != 0;
        ImGui::SetMouseCursor(horizontal && vertical
                                  ? ImGuiMouseCursor_ResizeNWSE
                                  : (horizontal ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS));
    }
}

bool RegionEditor::updateEditing(const Placement& placement, int imageWidth, int imageHeight)
{
    bool changed = false;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
        m_grab = grabAt(ImGui::GetMousePos(), placement);
        m_pressed = m_rect;
    }
    if (m_grab != ZoneNone && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 raw = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        const float scale = placement.scale > 0.0f ? placement.scale : 1.0f;
        applyDrag(ImVec2{raw.x / scale, raw.y / scale}, imageWidth, imageHeight);
        changed = true;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_grab = ZoneNone;
    }
    announceCursor(placement);

    return changed;
}

/// The close badge: taken when it is clicked. Hit-testing only - the badge
/// is DRAWN by drawBorder, last, so nothing paints over it.
bool RegionEditor::updateClose(const Placement& placement)
{
    if (!closeVisible(placement)) {
        return false;
    }
    const auto [topLeft, bottomRight] = screenRect(placement);
    const ImVec2 centre = closeCentre(topLeft, bottomRight);
    const ImVec2 mouse = ImGui::GetMousePos();

    const float dx = mouse.x - centre.x;
    const float dy = mouse.y - centre.y;
    if (dx * dx + dy * dy > CloseHitRadius * CloseHitRadius) {
        return false;
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_dismissed = true;
        m_grab = ZoneNone;

        return true;
    }

    return true;
}

bool RegionEditor::update(const Placement& placement, int imageWidth, int imageHeight)
{
    // A picture changed under the region since the last frame, and this is
    // the first frame in which the new one's placement is known.
    if (m_heldOnScreen) {
        restoreHeldPosition(placement, imageWidth, imageHeight);
    }
    // The badge is offered first: it sits on the band, and a press meant
    // for it must not become a drag of the region under it.
    const bool onClose = !m_arming && hasRegion() && updateClose(placement);
    const bool changed = m_arming
                             ? updateDrawing(placement, imageWidth, imageHeight)
                             : (hasRegion() && !onClose ? updateEditing(placement, imageWidth, imageHeight) : false);
    // One or the other, never both. While the picker is up the drag owns the
    // picture and wears a dashed rectangle; the band, the handles and the
    // badge are what a SETTLED region looks like, and drawing them over a
    // live drag was the whole difference from the desktop.
    if (m_arming) {
        drawPickerOverlay(placement, imageWidth, imageHeight);
    } else if (hasRegion()) {
        drawBorder(placement, imageWidth, imageHeight);
    }

    return changed;
}

}  // namespace sidescopes
