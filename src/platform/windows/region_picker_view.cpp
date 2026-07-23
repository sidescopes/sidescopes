#include "platform/windows/region_picker_view.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "platform/face_detection.h"
#include "platform/region_geometry.h"
#include "platform/windows/region_border_view.h"
#include "platform/windows/region_overlay_surface.h"

namespace sidescopes {

// ---------------------------------------------------------------------------
// Picker overlay
// ---------------------------------------------------------------------------

// One overlay per display; a pick anywhere is a pick there. Mode flags
// live per overlay and are switched in lockstep, the way the shared
// keyboard expects.
std::vector<PickerState*> g_pickers;

PickerState* pickerForWindow(HWND window)
{
    for (PickerState* picker : g_pickers) {
        if (picker->window == window) {
            return picker;
        }
    }
    return nullptr;
}

// This application's own top-level windows, except the overlays
// themselves.
std::vector<HWND> ownWindows()
{
    struct Collector
    {
        DWORD process = 0;
        std::vector<HWND>* windows = nullptr;
    };

    std::vector<HWND> windows;
    Collector collector{GetCurrentProcessId(), &windows};
    EnumWindows(
        [](HWND window, LPARAM context) -> BOOL {
            auto* state = reinterpret_cast<Collector*>(context);
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process != state->process || !IsWindowVisible(window)) {
                return TRUE;
            }
            if (pickerForWindow(window) != nullptr) {
                return TRUE;
            }
            if (window == g_border.window) {
                return TRUE;
            }
            state->windows->push_back(window);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));
    return windows;
}

std::vector<Gdiplus::RectF> ownWindowExclusions(const PickerState& picker)
{
    std::vector<Gdiplus::RectF> exclusions;
    for (HWND window : ownWindows()) {
        RECT rect{};
        if (!GetWindowRect(window, &rect)) {
            continue;
        }
        exclusions.emplace_back(static_cast<Gdiplus::REAL>(rect.left - picker.originX),
                                static_cast<Gdiplus::REAL>(rect.top - picker.originY),
                                static_cast<Gdiplus::REAL>(rect.right - rect.left),
                                static_cast<Gdiplus::REAL>(rect.bottom - rect.top));
    }
    return exclusions;
}

Gdiplus::RectF selectionRect(const PickerState& picker)
{
    Gdiplus::RectF selection = toRectF(
        selectionRectFromDrag(static_cast<double>(picker.dragStart.x), static_cast<double>(picker.dragStart.y),
                              static_cast<double>(picker.dragCurrent.x), static_cast<double>(picker.dragCurrent.y)));
    if (picker.constrained) {
        // The attached draw cannot leave its window.
        Gdiplus::RectF clamped;
        if (!Gdiplus::RectF::Intersect(clamped, selection, picker.constraintRect)) {
            clamped = Gdiplus::RectF(picker.constraintRect.X, picker.constraintRect.Y, 0, 0);
        }
        selection = clamped;
    }
    return selection;
}

namespace {

// The suggestion under the cursor. Windows are front-to-back, so the first one
// containing the point is the topmost there: the window that actually shows at
// that point wins over any window peeking out from behind it. Faces carry no
// depth, so the smallest box under the cursor wins for them instead.
int suggestionAtPoint(const PickerState& picker, POINT point)
{
    int best = -1;
    float bestArea = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < picker.suggestions.size(); ++index) {
        const Gdiplus::RectF& rect = picker.suggestions[index].first;
        if (!rect.Contains(static_cast<Gdiplus::REAL>(point.x), static_cast<Gdiplus::REAL>(point.y))) {
            continue;
        }
        if (!picker.facesMode) {
            return static_cast<int>(index);
        }

        const float area = rect.Width * rect.Height;
        if (area < bestArea) {
            bestArea = area;
            best = static_cast<int>(index);
        }
    }

    return best;
}

// The mode instruction: a primary line and a bracketed-keys line on a
// dark pill, placed where this application's own windows do not cover it
// - they float above the overlay, and a message half-hidden behind the
// scope window reads as a glitch.
void drawBanner(Gdiplus::Graphics& canvas, const PickerState& picker, const wchar_t* primary, const wchar_t* secondary,
                bool preferCenter, double scale)
{
    const Gdiplus::FontFamily family(L"Segoe UI");
    const Gdiplus::Font primaryFont(&family, static_cast<Gdiplus::REAL>(22 * scale), Gdiplus::FontStyleBold,
                                    Gdiplus::UnitPixel);
    const Gdiplus::Font secondaryFont(&family, static_cast<Gdiplus::REAL>(14 * scale), Gdiplus::FontStyleRegular,
                                      Gdiplus::UnitPixel);
    Gdiplus::RectF primarySize;
    Gdiplus::RectF secondarySize;
    canvas.MeasureString(primary, -1, &primaryFont, Gdiplus::PointF(0, 0), &primarySize);
    canvas.MeasureString(secondary, -1, &secondaryFont, Gdiplus::PointF(0, 0), &secondarySize);

    const auto width = static_cast<Gdiplus::REAL>(std::max(primarySize.Width, secondarySize.Width) + 48 * scale);
    const auto height = static_cast<Gdiplus::REAL>(primarySize.Height + secondarySize.Height + 30 * scale);
    const auto x = static_cast<Gdiplus::REAL>((static_cast<Gdiplus::REAL>(picker.width) - width) / 2);
    const auto topY = static_cast<Gdiplus::REAL>(80 * scale);
    const auto centerY = static_cast<Gdiplus::REAL>((static_cast<Gdiplus::REAL>(picker.height) - height) / 2);
    const auto lowY = static_cast<Gdiplus::REAL>(picker.height * 0.78 - height);
    const Gdiplus::REAL candidates[3] = {preferCenter ? centerY : topY, preferCenter ? topY : centerY, lowY};
    Gdiplus::RectF banner(x, candidates[0], width, height);
    const auto margin = static_cast<Gdiplus::REAL>(12 * scale);
    for (const Gdiplus::REAL candidate : candidates) {
        Gdiplus::RectF probe(x - margin, candidate - margin, width + 2 * margin, height + 2 * margin);
        bool covered = false;
        for (const Gdiplus::RectF& exclusion : picker.exclusions) {
            if (probe.IntersectsWith(exclusion)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            banner = Gdiplus::RectF(x, candidate, width, height);
            break;
        }
    }

    Gdiplus::GraphicsPath pill;
    const auto radius = static_cast<Gdiplus::REAL>(12 * scale);
    pill.AddArc(banner.X, banner.Y, radius * 2, radius * 2, 180, 90);
    pill.AddArc(banner.GetRight() - radius * 2, banner.Y, radius * 2, radius * 2, 270, 90);
    pill.AddArc(banner.GetRight() - radius * 2, banner.GetBottom() - radius * 2, radius * 2, radius * 2, 0, 90);
    pill.AddArc(banner.X, banner.GetBottom() - radius * 2, radius * 2, radius * 2, 90, 90);
    pill.CloseFigure();
    Gdiplus::SolidBrush pillBrush(Gdiplus::Color(140, 0, 0, 0));
    canvas.FillPath(&pillBrush, &pill);

    Gdiplus::SolidBrush primaryBrush(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::SolidBrush secondaryBrush(Gdiplus::Color(191, 255, 255, 255));
    canvas.DrawString(
        primary, -1, &primaryFont,
        Gdiplus::PointF(banner.X + (width - primarySize.Width) / 2, banner.Y + static_cast<Gdiplus::REAL>(10 * scale)),
        &primaryBrush);
    canvas.DrawString(
        secondary, -1, &secondaryFont,
        Gdiplus::PointF(banner.X + (width - secondarySize.Width) / 2,
                        banner.GetBottom() - secondarySize.Height - static_cast<Gdiplus::REAL>(10 * scale)),
        &secondaryBrush);
}

// A punched hole keeps a whisper of alpha where it must stay clickable:
// the picker owns every click on the overlay, but the darkened wash must
// lift off the rectangle being indicated.
void punchRect(Gdiplus::Graphics& canvas, const Gdiplus::RectF& rect)
{
    const Gdiplus::CompositingMode previous = canvas.GetCompositingMode();
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush whisper(Gdiplus::Color(13, 0, 0, 0));
    canvas.FillRectangle(&whisper, rect);
    canvas.SetCompositingMode(previous);
}

// The pin tool's overlay: the live selection frame during a drag, the
// instruction otherwise, over a wash too faint to tint the sampled color.
void paintPinScene(PickerState& picker, Gdiplus::Graphics& canvas, double scale, const Gdiplus::RectF& bounds)
{
    // No dim at all: judging a color through even a light wash
    // misleads. The single count of alpha is load-bearing and truly
    // invisible - fully transparent layered pixels are click-through
    // and any other value hit-tests, and pin clicks belong to the
    // overlay.
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush whisper(Gdiplus::Color(1, 0, 0, 0));
    canvas.FillRectangle(&whisper, bounds);
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    if (picker.dragging) {
        // A two-tone frame: the white line rides a dark halo so one
        // of the tones survives any undimmed background.
        const Gdiplus::RectF selection = selectionRect(picker);
        Gdiplus::Pen halo(Gdiplus::Color(179, 26, 26, 26), static_cast<Gdiplus::REAL>(3.0 * scale));
        canvas.DrawRectangle(&halo, selection);
        Gdiplus::Pen line(Gdiplus::Color(255, 255, 255, 255), static_cast<Gdiplus::REAL>(1.0 * scale));
        canvas.DrawRectangle(&line, selection);
    }
    if (!picker.dragging) {
        // Pinning is its own tool: no mode keys here and none of the
        // region modes lead back - crossing over midway would blur
        // what a click means.
        drawBanner(canvas, picker, L"Click or drag to pin a color", L"[Shift+click] pin and continue    [Esc] done",
                   false, scale);
    }
}

// The window- and face-picking overlay: a dim wash, every face outlined
// up front, and the suggestion under the cursor accented with its label.
void paintSuggestionScene(PickerState& picker, Gdiplus::Graphics& canvas, double scale, const Gdiplus::RectF& bounds)
{
    Gdiplus::SolidBrush dim(Gdiplus::Color(51, 0, 0, 0));
    canvas.FillRectangle(&dim, bounds);
    if (picker.facesMode) {
        // Faces are few and easy to miss: every found face is outlined
        // up front, so the answer is visible before any hovering. The
        // hovered one still gets the full accent treatment below.
        Gdiplus::Pen outline(Gdiplus::Color(217, 255, 255, 255), static_cast<Gdiplus::REAL>(1.5 * scale));
        for (int i = 0; i < static_cast<int>(picker.suggestions.size()); ++i) {
            if (i == picker.hoveredSuggestion) {
                continue;
            }
            punchRect(canvas, picker.suggestions[static_cast<std::size_t>(i)].first);
            canvas.DrawRectangle(&outline, picker.suggestions[static_cast<std::size_t>(i)].first);
        }
    }
    if (picker.hoveredSuggestion >= 0 && picker.hoveredSuggestion < static_cast<int>(picker.suggestions.size())) {
        // Only the rectangle under the cursor is shown, washed with an
        // accent like window selection in the screenshot interfaces.
        const auto& hovered = picker.suggestions[static_cast<std::size_t>(picker.hoveredSuggestion)];
        punchRect(canvas, hovered.first);
        if (picker.facesMode) {
            // A face target keeps the accent wash; a hovered window stays
            // natural - what shows is exactly what the scopes get.
            Gdiplus::SolidBrush wash(Gdiplus::Color(64, 0, 122, 255));
            canvas.FillRectangle(&wash, hovered.first);
        }
        // Transient indicators wear the warm tone: they are on screen for a
        // moment, and neutral grey vanished against bright content. Only
        // RESTING chrome must stay neutral beside the sampled pixels.
        Gdiplus::Pen frame(Gdiplus::Color(242, 255, 214, 140), static_cast<Gdiplus::REAL>(2.0 * scale));
        canvas.DrawRectangle(&frame, hovered.first);
        const Gdiplus::FontFamily family(L"Segoe UI");
        const Gdiplus::Font font(&family, static_cast<Gdiplus::REAL>(12 * scale), Gdiplus::FontStyleRegular,
                                 Gdiplus::UnitPixel);
        Gdiplus::SolidBrush text(Gdiplus::Color(255, 255, 255, 255));
        canvas.DrawString(hovered.second.c_str(), -1, &font,
                          Gdiplus::PointF(hovered.first.X + static_cast<Gdiplus::REAL>(6 * scale),
                                          hovered.first.Y + static_cast<Gdiplus::REAL>(6 * scale)),
                          &text);
    }
    if (picker.facesMode) {
        const wchar_t* secondary = L"[A] attach to a window    [D] draw    [Esc] full screen";
        if (!picker.suggestions.empty()) {
            drawBanner(canvas, picker, L"Attach to a face", secondary, false, scale);
        } else if (picker.facesScanned) {
            // Scanned, nothing found: the honest verdict. Before the scan
            // lands there is no banner - absence is not yet known.
            drawBanner(canvas, picker, L"No faces found on this screen", secondary, true, scale);
        }
    } else {
        drawBanner(canvas, picker, L"Click a window or drag a region inside it",
                   supportsFaceDetection() ? L"[F] attach to a face    [D] draw    [Esc] full screen"
                                           : L"[D] draw    [Esc] full screen",
                   false, scale);
    }
}

// The draw overlay: a dim wash with either the live selection punched
// through it or the instruction to start dragging.
void paintDrawScene(PickerState& picker, Gdiplus::Graphics& canvas, double scale, const Gdiplus::RectF& bounds)
{
    // Constrained (attached) drawing spotlights the target window: a hard
    // dim everywhere else, the window under the usual light veil, rimmed in
    // a neutral rim.
    Gdiplus::SolidBrush dim(Gdiplus::Color(picker.constrained ? 140 : 89, 0, 0, 0));
    canvas.FillRectangle(&dim, bounds);
    if (picker.constrained) {
        punchRect(canvas, picker.constraintRect);
        Gdiplus::Pen spotlight(Gdiplus::Color(230, 255, 214, 140), static_cast<Gdiplus::REAL>(1.5 * scale));
        canvas.DrawRectangle(&spotlight, picker.constraintRect);
    }
    if (picker.dragging) {
        const Gdiplus::RectF selection = selectionRect(picker);
        punchRect(canvas, selection);
        // The settled border's light-dark alternation, so the live drag
        // reads on pure white and pure black alike.
        Gdiplus::Pen backing(Gdiplus::Color(217, 26, 26, 26), static_cast<Gdiplus::REAL>(EdgeRing * scale));
        canvas.DrawRectangle(&backing, selection);
        Gdiplus::Pen frame(Gdiplus::Color(242, 247, 247, 247), static_cast<Gdiplus::REAL>(EdgeRing * scale));
        const Gdiplus::REAL pattern[2] = {static_cast<Gdiplus::REAL>(4.0), static_cast<Gdiplus::REAL>(4.0)};
        frame.SetDashPattern(pattern, 2);
        canvas.DrawRectangle(&frame, selection);
    } else if (picker.constrained) {
        const std::wstring primary = picker.constraintLabel.empty() ? std::wstring(L"Draw a region in the window")
                                                                    : L"Draw a region in " + picker.constraintLabel;
        drawBanner(canvas, picker, primary.c_str(), L"[Esc] cancel", false, scale);
    } else {
        const wchar_t* secondary = L"[Esc] full screen";
        if (!picker.windows.empty() && supportsFaceDetection()) {
            secondary = L"[A] attach to a window    [F] attach to a face    [Esc] full screen";
        } else if (!picker.windows.empty()) {
            secondary = L"[A] attach to a window    [Esc] full screen";
        }
        drawBanner(canvas, picker, L"Drag to draw a region", secondary, false, scale);
    }
}

// The overlay scene proper, described in full every time: the caller
// owns the surface, the clip that limits what actually rasterizes, and
// the push to the compositor.
void paintPickerScene(PickerState& picker, Gdiplus::Graphics& canvas, double scale)
{
    const Gdiplus::RectF bounds(0, 0, static_cast<Gdiplus::REAL>(picker.width),
                                static_cast<Gdiplus::REAL>(picker.height));
    if (picker.pinMode) {
        paintPinScene(picker, canvas, scale, bounds);
    } else if (!picker.drawMode && !picker.pickDragging) {
        paintSuggestionScene(picker, canvas, scale, bounds);
    } else {
        paintDrawScene(picker, canvas, scale, bounds);
    }
}

}  // namespace

// Repaints the overlay. With a dirty rectangle, only that area is redrawn
// and pushed - the clip keeps the rasterizer inside the changed sliver,
// which is what makes a display-sized layered window affordable to update
// per mouse move.
void paintPicker(PickerState& picker, const Gdiplus::RectF* dirty)
{
    if (!ensureGdiplus()) {
        return;
    }
    if (!picker.surface || !picker.surface->valid()) {
        picker.surface = std::make_unique<LayeredSurface>(picker.width, picker.height);
        if (!picker.surface->valid()) {
            return;
        }
        dirty = nullptr;  // a fresh surface has no previous frame to reuse
    }
    Gdiplus::Graphics canvas(picker.surface->dc());
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    canvas.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    const double scale = uiScale(picker.window);
    const Gdiplus::RectF bounds(0, 0, static_cast<Gdiplus::REAL>(picker.width),
                                static_cast<Gdiplus::REAL>(picker.height));
    if (dirty) {
        canvas.SetClip(*dirty);
    }
    // The cached surface still holds the previous frame; painting starts
    // from transparency the way it did when every paint got a fresh DIB.
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush erase(Gdiplus::Color(0, 0, 0, 0));
    canvas.FillRectangle(&erase, dirty ? *dirty : bounds);
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    paintPickerScene(picker, canvas, scale);
    if (dirty) {
        canvas.ResetClip();
        picker.surface->pushDirty(picker.window, picker.originX, picker.originY, *dirty);
    } else {
        picker.surface->push(picker.window, picker.originX, picker.originY);
    }
}

namespace {

// The per-move repaint of a live selection drag. Between two frames the
// selection interiors agree (the same whisper punch) and the frame
// strokes ride each rectangle's boundary - which never enters the shared
// interior - so everything comfortably inside both rectangles is already
// correct on the surface. Only the ring around that core is repainted,
// and it is pushed as four bands: uploading the union's bounding box
// would send the compositor megabytes it already holds, exactly what a
// fast full-screen drag cannot afford.
void paintPickerSelectionDelta(PickerState& picker, const Gdiplus::RectF& previous, const Gdiplus::RectF& current,
                               double scale)
{
    if (!ensureGdiplus()) {
        return;
    }
    if (!picker.surface || !picker.surface->valid()) {
        paintPicker(picker);
        return;
    }
    // Covers the frame stroke and its antialiasing on either rim.
    const auto margin = static_cast<Gdiplus::REAL>(4 * scale);
    Gdiplus::RectF outer;
    Gdiplus::RectF::Union(outer, previous, current);
    outer.Inflate(margin, margin);
    Gdiplus::RectF core;
    bool haveCore = Gdiplus::RectF::Intersect(core, previous, current) != FALSE;
    if (haveCore) {
        core.Inflate(-2 * margin, -2 * margin);
        haveCore = core.Width > 0 && core.Height > 0;
    }
    if (!haveCore) {
        paintPicker(picker, &outer);
        return;
    }
    Gdiplus::Graphics canvas(picker.surface->dc());
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    canvas.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::Region ring(outer);
    ring.Exclude(core);
    canvas.SetClip(&ring);
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush erase(Gdiplus::Color(0, 0, 0, 0));
    canvas.FillRectangle(&erase, outer);
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    paintPickerScene(picker, canvas, scale);
    canvas.ResetClip();
    const Gdiplus::RectF bands[4] = {{outer.X, outer.Y, outer.Width, core.Y - outer.Y},
                                     {outer.X, core.GetBottom(), outer.Width, outer.GetBottom() - core.GetBottom()},
                                     {outer.X, core.Y, core.X - outer.X, core.Height},
                                     {core.GetRight(), core.Y, outer.GetRight() - core.GetRight(), core.Height}};
    for (const Gdiplus::RectF& band : bands) {
        if (band.Width <= 0 || band.Height <= 0) {
            continue;
        }
        picker.surface->pushDirty(picker.window, picker.originX, picker.originY, band);
    }
}

}  // namespace

// 0 = attach to a window, 1 = draw, 2 = attach to a face, 3 = pin
// colors. Face mode is available even when no face was found: the honest
// answer is the empty overlay saying so, not a key that silently does
// nothing.
void switchPickerMode(int mode)
{
    const bool draw = mode == 1;
    const bool faces = mode == 2;
    const bool pin = mode == 3;
    // Region picking and color pinning are separate tools; a pick never
    // crosses between the families midway.
    if (!g_pickers.empty() && g_pickers.front()->pinMode != pin) {
        return;
    }
    for (PickerState* picker : g_pickers) {
        if (picker->drawMode == draw && picker->facesMode == faces && picker->pinMode == pin) {
            continue;
        }
        picker->drawMode = draw;
        picker->facesMode = faces;
        picker->pinMode = pin;
        picker->suggestions = faces ? picker->faces : picker->windows;
        picker->hoveredSuggestion = -1;
        picker->dragging = false;
        picker->pickDragging = false;
        picker->constrained = false;
        picker->selectionPainted = false;
        paintPicker(*picker);
    }
}

namespace {

LRESULT pickerOnSetCursor(PickerState& picker)
{
    if (picker.pinMode) {
        SetCursor(g_pinCursor ? g_pinCursor : LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    }
    // Window mode draws as readily as it clicks; only face mode is
    // click-only and keeps the hand.
    SetCursor(LoadCursorW(nullptr, picker.facesMode && !picker.pickDragging ? IDC_HAND : IDC_CROSS));
    return TRUE;
}

// A pin-mode drag: the swatch rides the cursor image itself, so motion
// alone needs no repaint - only an active selection drag does, and then
// only its changed sliver.
void pickerPinDrag(PickerState& picker, HWND window, WPARAM wParam, POINT point)
{
    // The swatch rides the cursor image itself; motion needs
    // no repaint here, only an active drag does.
    if ((wParam & MK_LBUTTON) == 0) {
        return;
    }
    const double scale = uiScale(window);
    const Gdiplus::RectF previous = selectionRect(picker);
    picker.dragCurrent = point;
    const double threshold = 4 * scale;
    if (!picker.dragging && (std::abs(picker.dragCurrent.x - picker.dragStart.x) > threshold ||
                             std::abs(picker.dragCurrent.y - picker.dragStart.y) > threshold)) {
        picker.dragging = true;
        paintPicker(picker);  // full: the banner leaves
        return;
    }
    if (!picker.dragging) {
        return;
    }
    Gdiplus::RectF changed = previous;
    Gdiplus::RectF::Union(changed, changed, selectionRect(picker));
    const auto margin = static_cast<Gdiplus::REAL>(4 * scale);
    changed.Inflate(margin, margin);
    paintPicker(picker, &changed);
}

// A draw-mode drag: past a few points of travel the selection becomes
// real and repaints as a delta once it has a previous frame to reuse.
void pickerDrawDrag(PickerState& picker, HWND window, WPARAM wParam, POINT point)
{
    if ((wParam & MK_LBUTTON) == 0) {
        return;
    }
    picker.dragCurrent = point;
    // A real drag only starts after a few points of travel,
    // so a stray click never flashes a tiny selection.
    const double scale = uiScale(window);
    const double threshold = 4 * scale;
    if (!picker.dragging && (std::abs(picker.dragCurrent.x - picker.dragStart.x) > threshold ||
                             std::abs(picker.dragCurrent.y - picker.dragStart.y) > threshold)) {
        picker.dragging = true;
    }
    // Nothing on screen changes until the drag is real:
    // the pre-threshold scene is the banner one already
    // showing.
    if (picker.dragging) {
        const Gdiplus::RectF selection = selectionRect(picker);
        if (picker.selectionPainted) {
            paintPickerSelectionDelta(picker, picker.paintedSelection, selection, scale);
        } else {
            paintPicker(picker);  // full: the banner leaves
        }
        picker.paintedSelection = selection;
        picker.selectionPainted = true;
    }
}

// Window/face mode without a drag: the highlight follows the suggestion under
// the cursor.
void pickerHoverMove(PickerState& picker, POINT point)
{
    const int hovered = suggestionAtPoint(picker, point);
    if (hovered != picker.hoveredSuggestion) {
        picker.hoveredSuggestion = hovered;
        paintPicker(picker);
    }
}

// Window mode: the highlight follows the cursor until a held button has
// travelled far enough, then the gesture becomes an attached draw within
// the window under the drag's start - the spotlight and the clamp follow
// that window for the whole gesture.
void pickerWindowDrag(PickerState& picker, HWND window, WPARAM wParam, POINT point)
{
    if ((wParam & MK_LBUTTON) == 0 || picker.facesMode) {
        pickerHoverMove(picker, point);
        return;
    }
    picker.dragCurrent = point;
    const double threshold = 4 * uiScale(window);
    if (std::abs(static_cast<double>(picker.dragCurrent.x - picker.dragStart.x)) <= threshold &&
        std::abs(static_cast<double>(picker.dragCurrent.y - picker.dragStart.y)) <= threshold) {
        return;
    }
    const int target = suggestionAtPoint(picker, picker.dragStart);
    picker.constrained = target >= 0;
    if (target >= 0) {
        picker.constraintRect = picker.suggestions[static_cast<std::size_t>(target)].first;
        picker.constraintLabel = picker.suggestions[static_cast<std::size_t>(target)].second;
    }
    picker.pickDragging = true;
    picker.dragging = true;
    paintPicker(picker);  // full: the draw scene arrives
}

LRESULT pickerOnMouseMove(PickerState& picker, HWND window, WPARAM wParam, LPARAM lParam)
{
    const POINT point{static_cast<int>(static_cast<short>(LOWORD(lParam))),
                      static_cast<int>(static_cast<short>(HIWORD(lParam)))};
    if (picker.pinMode) {
        pickerPinDrag(picker, window, wParam, point);
    } else if (picker.drawMode || picker.pickDragging) {
        pickerDrawDrag(picker, window, wParam, point);
    } else {
        pickerWindowDrag(picker, window, wParam, point);
    }
    return 0;
}

// A pin-mode release: a real drag pins its rectangle, a click pins the
// point itself, and the click's Shift decides whether picking continues.
void pickerPinUp(PickerState& picker, HWND window, WPARAM wParam, POINT point)
{
    const double scale = uiScale(window);
    picker.dragCurrent = point;
    const Gdiplus::RectF selection = selectionRect(picker);
    const double minimum = 8 * scale;
    if (picker.dragging && selection.Width > minimum && selection.Height > minimum) {
        picker.pinnedSample = selection;
        picker.pinnedIsPoint = false;
    } else {
        // A plain click pins the point itself, so the pinned color is
        // exactly what the live cursor readout showed; averaging a patch
        // here would fade pins taken over a small subject. Dragging a
        // rectangle is the explicit way to average a swatch.
        picker.pinnedPoint = Gdiplus::PointF(static_cast<Gdiplus::REAL>(point.x), static_cast<Gdiplus::REAL>(point.y));
        picker.pinnedIsPoint = true;
    }
    // The click's Shift carries the per-pin decision: pin
    // and keep picking, or pin and be done.
    picker.pinnedKeepOpen = (wParam & MK_SHIFT) != 0;
    picker.pinnedReady = true;
    if (picker.dragging) {
        picker.dragging = false;
        paintPicker(picker);  // full: the frame leaves, the banner returns
    }
}

// A window/face release: a hit confirms that suggestion and finishes the
// pick; a miss keeps the picker open.
void pickerWindowUp(PickerState& picker, POINT point)
{
    const int hovered = suggestionAtPoint(picker, point);
    if (hovered < 0) {
        return;  // a miss keeps the picker open
    }
    picker.picked = true;
    picker.confirmedRect = picker.suggestions[static_cast<std::size_t>(hovered)].first;
    picker.finished = true;
}

// A draw-mode release: a selection past the minimum size confirms and
// finishes; a stray tap just repaints the banner.
void pickerDrawUp(PickerState& picker, HWND window, POINT point)
{
    picker.dragCurrent = point;
    if (picker.dragging) {
        const Gdiplus::RectF selection = selectionRect(picker);
        picker.dragging = false;
        picker.selectionPainted = false;
        const double minimum = 8 * uiScale(window);
        if (selection.Width > minimum && selection.Height > minimum) {
            picker.picked = true;
            picker.confirmedRect = selection;
            picker.finished = true;
        } else {
            // A stray micro-drag keeps the picker open.
            if (picker.pickDragging) {
                picker.pickDragging = false;
                picker.constrained = false;
            }
            paintPicker(picker);
        }
    }
}

LRESULT pickerOnLButtonUp(PickerState& picker, HWND window, WPARAM wParam, LPARAM lParam)
{
    ReleaseCapture();
    const POINT point{static_cast<int>(static_cast<short>(LOWORD(lParam))),
                      static_cast<int>(static_cast<short>(HIWORD(lParam)))};
    if (picker.pinMode) {
        pickerPinUp(picker, window, wParam, point);
    } else if (!picker.drawMode && !picker.pickDragging) {
        pickerWindowUp(picker, point);
    } else {
        pickerDrawUp(picker, window, point);
    }
    return 0;
}

LRESULT pickerOnKeyDown(PickerState& picker, WPARAM wParam)
{
    const int key = static_cast<int>(wParam);
    if (key == VK_ESCAPE) {
        picker.picked = false;
        picker.finished = true;
        return 0;
    }
    // Pinning is its own tool: while it is up, only ESC speaks.
    if (picker.pinMode) {
        return 0;
    }
    if (key == 'A') {
        switchPickerMode(0);
    }
    if (key == 'D') {
        switchPickerMode(1);
    }
    if (key == 'F' && supportsFaceDetection()) {
        switchPickerMode(2);
    }
    return 0;
}

}  // namespace

LRESULT CALLBACK pickerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    PickerState* state = pickerForWindow(window);
    if (!state) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    PickerState& picker = *state;
    switch (message) {
    case WM_SETCURSOR:
        return pickerOnSetCursor(picker);
    case WM_MOUSEMOVE:
        return pickerOnMouseMove(picker, window, wParam, lParam);
    case WM_LBUTTONDOWN:
        picker.dragStart = {static_cast<int>(static_cast<short>(LOWORD(lParam))),
                            static_cast<int>(static_cast<short>(HIWORD(lParam)))};
        picker.dragCurrent = picker.dragStart;
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP:
        return pickerOnLButtonUp(picker, window, wParam, lParam);
    case WM_KEYDOWN:
        return pickerOnKeyDown(picker, wParam);
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace sidescopes
