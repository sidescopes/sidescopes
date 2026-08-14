#include "web/region_editor.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {
namespace {

// src/platform/macos/region_border_view.h's own numbers. The desktop border
// is a window on the desktop and cannot run here, but its geometry can, and
// a demo whose border is a near-miss of the application's teaches the wrong
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
    const ImVec2 topLeft{placement.origin.x + static_cast<float>(m_rect.x) * placement.scale,
                         placement.origin.y + static_cast<float>(m_rect.y) * placement.scale};
    const ImVec2 bottomRight{topLeft.x + static_cast<float>(m_rect.width) * placement.scale,
                             topLeft.y + static_cast<float>(m_rect.height) * placement.scale};

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
    const ImVec4 bands[4] = {
        {outerTopLeft.x, outerTopLeft.y, outer.x, holeTopLeft.y},
        {outerTopLeft.x, holeBottomRight.y, outer.x, outer.y},
        {outerTopLeft.x, holeTopLeft.y, holeTopLeft.x, holeBottomRight.y},
        {holeBottomRight.x, holeTopLeft.y, outer.x, holeBottomRight.y},
    };
    const float height = outer.y - outerTopLeft.y;
    for (const ImVec4& band : bands) {
        if (band.z <= band.x || band.w <= band.y) {
            continue;
        }
        draw->PushClipRect(ImVec2{band.x, band.y}, ImVec2{band.z, band.w}, true);
        draw->AddRectFilled(ImVec2{band.x, band.y}, ImVec2{band.z, band.w}, grey(0.1f, 0.45f));
        // An integer step keeps the diagonal spacing exact, as the desktop's
        // own loop does; a float counter accumulates rounding across a band.
        const float start = outerTopLeft.x - height;
        for (int step = 0; start + static_cast<float>(step) * 10.0f < outer.x; ++step) {
            const float x = start + static_cast<float>(step) * 10.0f;
            draw->AddLine(ImVec2{x, outer.y}, ImVec2{x + height, outerTopLeft.y}, grey(0.9f, 0.45f), 4.0f);
        }
        draw->PopClipRect();
    }
}

/// The measured edge: a dark ring from the region outwards, with white
/// dashes riding over it, so one tone survives any background. The
/// screenshot tools' marching ants, standing still.
void drawMeasuredEdge(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight)
{
    draw->AddRect(ImVec2{topLeft.x - EdgeRing, topLeft.y - EdgeRing},
                  ImVec2{bottomRight.x + EdgeRing, bottomRight.y + EdgeRing}, grey(0.1f, 0.85f), 0.0f, 0,
                  EdgeRing * 2.0f);
    constexpr float Dash = 4.0f;
    for (float x = topLeft.x; x < bottomRight.x; x += Dash * 2.0f) {
        const float to = std::min(x + Dash, bottomRight.x);
        draw->AddLine(ImVec2{x, topLeft.y}, ImVec2{to, topLeft.y}, grey(0.97f, 0.95f), EdgeRing);
        draw->AddLine(ImVec2{x, bottomRight.y}, ImVec2{to, bottomRight.y}, grey(0.97f, 0.95f), EdgeRing);
    }
    for (float y = topLeft.y; y < bottomRight.y; y += Dash * 2.0f) {
        const float to = std::min(y + Dash, bottomRight.y);
        draw->AddLine(ImVec2{topLeft.x, y}, ImVec2{topLeft.x, to}, grey(0.97f, 0.95f), EdgeRing);
        draw->AddLine(ImVec2{bottomRight.x, y}, ImVec2{bottomRight.x, to}, grey(0.97f, 0.95f), EdgeRing);
    }
}

}  // namespace

bool RegionEditor::takeDismissed()
{
    const bool dismissed = m_dismissed;
    m_dismissed = false;

    return dismissed;
}

ImVec2 RegionEditor::closeCentre(const ImVec2& topLeft, const ImVec2& bottomRight)
{
    // The band's outer top corner, pulled in a touch so the disc mostly
    // rides the band - the desktop's own placement, mirrored for a
    // downward y.
    return ImVec2{bottomRight.x + BorderPad - CloseCornerInset, topLeft.y - BorderPad + CloseCornerInset + EdgeRing};
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
    draw->AddCircleFilled(centre, CloseRadius, grey(0.1f, 0.85f));
    draw->AddCircle(centre, CloseRadius, grey(0.97f, 0.95f), 0, 1.0f);

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
        draw->AddCircleFilled(end, CrossThickness * 0.5f, ink);
    }
}

void RegionEditor::drawBorder(const Placement& placement, int imageWidth, int imageHeight) const
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 topLeft{placement.origin.x + static_cast<float>(m_rect.x) * placement.scale,
                         placement.origin.y + static_cast<float>(m_rect.y) * placement.scale};
    const ImVec2 bottomRight{topLeft.x + static_cast<float>(m_rect.width) * placement.scale,
                             topLeft.y + static_cast<float>(m_rect.height) * placement.scale};

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
        draw->AddCircleFilled(point, HandleRadius, grey(0.78f, 1.0f));
        draw->AddCircle(point, HandleRadius, grey(0.1f, 0.7f), 0, 2.0f);
        draw->AddCircle(point, HandleRadius, grey(0.97f, 0.95f), 0, 1.0f);
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
    const ImVec2 topLeft{placement.origin.x + static_cast<float>(m_rect.x) * placement.scale,
                         placement.origin.y + static_cast<float>(m_rect.y) * placement.scale};
    const ImVec2 bottomRight{topLeft.x + static_cast<float>(m_rect.width) * placement.scale,
                             topLeft.y + static_cast<float>(m_rect.height) * placement.scale};
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
