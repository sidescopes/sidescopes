// Region picking and the interactive region border on Windows: layered
// per-pixel-alpha popup windows painted with GDI+ and pushed through
// UpdateLayeredWindow. Both run on the application's main thread, whose
// GLFW event loop pumps their messages alongside its own.
//
// Both overlays opt out of screen capture (SetWindowDisplayAffinity):
// duplication has no per-application exclusion the way ScreenCaptureKit
// does, so each of our windows excludes itself instead.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/region_selection.h"

#include <windows.h>

// The COM base types GDI+ leans on (IStream, PROPID) are among what
// WIN32_LEAN_AND_MEAN strips out of windows.h.
#include <objidl.h>

// The GDI+ headers also call min and max unqualified, and NOMINMAX
// removed the macros they expect; the standard functions stand in.
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

// Windows 10 2004; absent from older SDKs.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#include "core/diagnostics.h"
#include "platform/desktop.h"
#include "platform/region_geometry.h"
#include "platform/windows/capture_visibility.h"
#include "platform/windows/region_border_view.h"
#include "platform/windows/region_overlay_surface.h"
#include "platform/windows/region_picker_view.h"
#include "platform/windows/wide_strings.h"

namespace sidescopes {

HCURSOR g_pinCursor = nullptr;

namespace {

// The pin cursor's swatch color, pushed by the application once per
// frame; sampled from the capture stream so the swatch previews exactly
// what a click would pin.
std::optional<FloatColor> g_pinChipColor;

// The pin cursor: crosshair and preview swatch drawn into the CURSOR
// itself. A swatch painted into the overlay always trails the hardware
// cursor by a composition frame - the mouse image rides its own
// zero-latency plane, so the swatch does too. The crosshair is a
// two-tone grey, the look the system crosshair only has over dimmed
// content.
constexpr double PinCursorHotspot = 12.0;  // crosshair center, points
constexpr double PinCursorArm = 8.0;
constexpr double PinCursorGap = 2.0;
constexpr double PinSwatchOffset = 7.0;  // from the hotspot, points
constexpr double PinSwatchSize = 13.0;

HCURSOR buildPinCursor(double scale, const std::optional<FloatColor>& color)
{
    const int side = static_cast<int>((PinCursorHotspot + PinSwatchOffset + PinSwatchSize) * scale + 4);
    const auto hotspot = static_cast<int>(PinCursorHotspot * scale);
    LayeredSurface surface(side, side);
    if (!surface.valid()) {
        return nullptr;
    }
    Gdiplus::Graphics canvas(surface.dc());
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const auto center = static_cast<Gdiplus::REAL>(hotspot);
    const auto arm = static_cast<Gdiplus::REAL>(PinCursorArm * scale);
    const auto gap = static_cast<Gdiplus::REAL>(PinCursorGap * scale);
    const auto stroke = [&](Gdiplus::Pen& pen) {
        canvas.DrawLine(&pen, center, center - arm, center, center - gap);
        canvas.DrawLine(&pen, center, center + gap, center, center + arm);
        canvas.DrawLine(&pen, center - arm, center, center - gap, center);
        canvas.DrawLine(&pen, center + gap, center, center + arm, center);
    };
    Gdiplus::Pen dark(Gdiplus::Color(217, 26, 26, 26), static_cast<Gdiplus::REAL>(3.2 * scale));
    stroke(dark);
    Gdiplus::Pen light(Gdiplus::Color(242, 205, 205, 205), static_cast<Gdiplus::REAL>(1.5 * scale));
    stroke(light);

    if (color) {
        const auto offset = static_cast<Gdiplus::REAL>(PinSwatchOffset * scale);
        const auto size = static_cast<Gdiplus::REAL>(PinSwatchSize * scale);
        const Gdiplus::RectF swatch(center + offset, center + offset, size, size);
        Gdiplus::SolidBrush fill(
            Gdiplus::Color(255, static_cast<BYTE>(color->r), static_cast<BYTE>(color->g), static_cast<BYTE>(color->b)));
        canvas.FillRectangle(&fill, swatch);
        Gdiplus::Pen rim(Gdiplus::Color(179, 26, 26, 26), static_cast<Gdiplus::REAL>(2.0 * scale));
        canvas.DrawRectangle(&rim, swatch);
        Gdiplus::Pen ring(Gdiplus::Color(242, 247, 247, 247), static_cast<Gdiplus::REAL>(1.0 * scale));
        canvas.DrawRectangle(&ring, swatch);
    }

    // CreateIconIndirect copies both bitmaps; the surface and the mask
    // are ours to free.
    HBITMAP mask = CreateBitmap(side, side, 1, 1, nullptr);
    if (!mask) {
        return nullptr;
    }
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(hotspot);
    info.yHotspot = static_cast<DWORD>(hotspot);
    info.hbmMask = mask;
    info.hbmColor = surface.bitmapHandle();
    HCURSOR cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(mask);
    return cursor;
}

HWND createOverlayWindow(const wchar_t* className, WNDPROC procedure, DWORD exStyle, UINT classStyle)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = classStyle;
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = nullptr;   // WM_SETCURSOR decides
    RegisterClassExW(&windowClass);  // idempotent; re-registration fails harmlessly
    HWND window = CreateWindowExW(exStyle, className, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    // Our own surfaces must never reach the scopes; duplication has no
    // application-level exclusion, so each window excludes itself unless
    // the visibility toggle holds. Best effort: unsupported before
    // Windows 10 2004.
    if (window) {
        SetWindowDisplayAffinity(window, captureWindowsVisible() ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
    }
    return window;
}

// Builds one display's picker overlay: its geometry, the window and face
// suggestions in overlay-local pixels, the initial mode, and a shown
// layered window. Returns nullptr when the display vanished between
// enumeration and now, or the window could not be created.
PickerState* createPicker(const PickerDisplay& entry, bool draw, bool faces, bool pin)
{
    const auto geometry = geometryOfDisplay(entry.displayId);
    if (!geometry) {
        return nullptr;  // gone between enumeration and now
    }

    auto* picker = new PickerState;
    picker->displayId = entry.displayId;
    picker->originX = static_cast<int>(geometry->originX);
    picker->originY = static_cast<int>(geometry->originY);
    picker->width = static_cast<int>(geometry->widthPoints);
    picker->height = static_cast<int>(geometry->heightPoints);
    for (const SuggestedRegion& suggestion : entry.windows) {
        picker->windows.emplace_back(toRectF(localRectFromRegion(suggestion.region, picker->width, picker->height)),
                                     wideFromUtf8(suggestion.label));
    }
    for (const SuggestedRegion& suggestion : entry.faces) {
        picker->faces.emplace_back(toRectF(localRectFromRegion(suggestion.region, picker->width, picker->height)),
                                   wideFromUtf8(suggestion.label));
    }
    picker->drawMode = draw;
    picker->facesMode = faces;
    picker->facesScanned = entry.facesScanned;
    picker->pinMode = pin;
    if (!draw && !pin) {
        picker->suggestions = faces ? picker->faces : picker->windows;
    }

    picker->window = createOverlayWindow(L"SidescopesPickerOverlay", pickerProc,
                                         WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, 0);
    if (!picker->window) {
        delete picker;
        return nullptr;
    }
    SetWindowPos(picker->window, HWND_TOPMOST, picker->originX, picker->originY, picker->width, picker->height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    return picker;
}

// Once the overlays exist, float this application's own windows above
// them, give the keyboard to the display under the cursor, and paint
// every overlay for the first time.
void presentPickers()
{
    // This application's own visible windows float above the overlays for
    // the duration: they stay undimmed and clickable by ordinary window
    // compositing. Their rectangles still feed the banner placement,
    // which avoids sitting beneath them. They are topmost already (the
    // scope window floats), so there is no level to restore afterwards.
    for (HWND window : ownWindows()) {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // The keyboard starts on the display under the cursor - that is where
    // the user's attention is - and follows clicks after.
    PickerState* keyPicker = g_pickers.front();
    const uint32_t cursorDisplay = displayUnderCursor().value_or(0);
    for (PickerState* picker : g_pickers) {
        picker->exclusions = ownWindowExclusions(*picker);
        if (picker->displayId == cursorDisplay) {
            keyPicker = picker;
        }
    }
    SetForegroundWindow(keyPicker->window);
    SetFocus(keyPicker->window);
    for (PickerState* picker : g_pickers) {
        paintPicker(*picker);
    }
}

// A pin ready on any overlay is reported once and cleared; at most one is
// outstanding per poll.
void collectPinnedSample(RegionPickPoll& poll)
{
    for (PickerState* picker : g_pickers) {
        if (!picker->pinnedReady) {
            continue;
        }
        picker->pinnedReady = false;
        if (picker->pinnedIsPoint) {
            // Overlay-local pixels to display percent, the same mapping
            // regionFromLocalRect applies to a rectangle's edges.
            poll.pinnedPoint = DisplayPoint{picker->pinnedPoint.X / static_cast<double>(picker->width) * 100.0,
                                            picker->pinnedPoint.Y / static_cast<double>(picker->height) * 100.0};
        } else {
            poll.pinnedSample = regionFromLocalRect(toLocalRect(picker->pinnedSample), picker->width, picker->height);
        }
        poll.pinnedKeepOpen = picker->pinnedKeepOpen;
        poll.displayId = picker->displayId;
        break;
    }
}

// The banner dodges this application's own windows; their rectangles
// refresh on a gentle cadence - nothing visual tracks them, so per-frame
// repaints would be waste.
void refreshBannerExclusions()
{
    static ULONGLONG lastExclusionRefresh = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastExclusionRefresh <= 200) {
        return;
    }
    lastExclusionRefresh = now;
    for (PickerState* picker : g_pickers) {
        std::vector<Gdiplus::RectF> exclusions = ownWindowExclusions(*picker);
        if (exclusions.size() != picker->exclusions.size() ||
            !std::equal(exclusions.begin(), exclusions.end(), picker->exclusions.begin(),
                        [](const Gdiplus::RectF& a, const Gdiplus::RectF& b) { return a.Equals(b); })) {
            picker->exclusions = std::move(exclusions);
            paintPicker(*picker);
        }
    }
}

// Any overlay finishing - a confirm there, or ESC anywhere - ends the
// pick on every display. Returns true when the pick has ended, with the
// result recorded in poll and every overlay destroyed.
bool finishRegionPick(RegionPickPoll& poll)
{
    PickerState* finishedPicker = nullptr;
    for (PickerState* picker : g_pickers) {
        if (picker->finished) {
            finishedPicker = picker;
            break;
        }
    }
    if (!finishedPicker) {
        return false;
    }
    poll.finished = true;
    poll.displayId = finishedPicker->displayId;
    if (finishedPicker->picked) {
        poll.confirmed = regionFromLocalRect(toLocalRect(finishedPicker->confirmedRect), finishedPicker->width,
                                             finishedPicker->height);
    }
    for (PickerState* picker : g_pickers) {
        DestroyWindow(picker->window);
        delete picker;
    }
    g_pickers.clear();
    if (g_pinCursor) {
        DestroyIcon(g_pinCursor);
        g_pinCursor = nullptr;
    }
    return true;
}

// The cursor is only ever on one display, so at most one overlay has
// something to preview.
void collectRegionPreview(RegionPickPoll& poll)
{
    for (PickerState* picker : g_pickers) {
        if (picker->pinMode) {
            break;  // pin modes never preview a region
        }
        if (!picker->drawMode) {
            if (picker->hoveredSuggestion >= 0 &&
                picker->hoveredSuggestion < static_cast<int>(picker->suggestions.size())) {
                poll.preview = regionFromLocalRect(
                    toLocalRect(picker->suggestions[static_cast<std::size_t>(picker->hoveredSuggestion)].first),
                    picker->width, picker->height);
                poll.displayId = picker->displayId;
                break;
            }
        } else if (picker->dragging) {
            const Gdiplus::RectF selection = selectionRect(*picker);
            const double minimum = 8 * uiScale(picker->window);
            if (selection.Width > minimum && selection.Height > minimum) {
                poll.preview = regionFromLocalRect(toLocalRect(selection), picker->width, picker->height);
                poll.displayId = picker->displayId;
                break;
            }
        }
    }
}

}  // namespace

bool beginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode initialMode)
{
    if (!g_pickers.empty()) {
        return false;  // one picker at a time
    }

    // An attach with nothing to attach to anywhere opens as drawing, like
    // before, but the decision is global so every display shows the same
    // mode.
    bool anyWindows = false;
    for (const PickerDisplay& entry : displays) {
        anyWindows |= !entry.windows.empty();
    }
    const bool pin = initialMode == RegionPickerMode::PinColor;
    const bool draw = !pin && (initialMode == RegionPickerMode::DrawGlobal ||
                               (initialMode == RegionPickerMode::AttachWindow && !anyWindows));
    const bool faces = initialMode == RegionPickerMode::AttachFace;

    for (const PickerDisplay& entry : displays) {
        if (PickerState* picker = createPicker(entry, draw, faces, pin)) {
            g_pickers.push_back(picker);
        }
    }
    if (g_pickers.empty()) {
        return false;
    }

    presentPickers();
    return true;
}

RegionPickPoll pollRegionPick()
{
    RegionPickPoll poll;
    if (g_pickers.empty()) {
        return poll;
    }
    poll.active = true;
    // Mode flags come first: the finishing poll returns early below, and
    // the caller needs them to know a pin-mode finish never means a
    // region change. The pickers switch modes in lockstep; the front one
    // speaks for all.
    poll.pinMode = g_pickers.front()->pinMode;
    poll.attachesToWindow =
        !g_pickers.front()->pinMode && !g_pickers.front()->drawMode && !g_pickers.front()->facesMode;
    collectPinnedSample(poll);
    refreshBannerExclusions();
    if (finishRegionPick(poll)) {
        return poll;
    }
    collectRegionPreview(poll);
    return poll;
}

void cancelRegionPick()
{
    if (g_pickers.empty()) {
        return;
    }
    g_pickers.front()->picked = false;
    g_pickers.front()->finished = true;
}

void setRegionPickMode(RegionPickerMode mode)
{
    switchPickerMode(mode == RegionPickerMode::DrawGlobal   ? 1
                     : mode == RegionPickerMode::AttachFace ? 2
                     : mode == RegionPickerMode::PinColor   ? 3
                                                            : 0);
}

void updatePickerFaces(uint32_t displayId, const std::vector<SuggestedRegion>& faces)
{
    for (PickerState* picker : g_pickers) {
        if (picker->displayId != displayId) {
            continue;
        }
        picker->faces.clear();
        for (const SuggestedRegion& suggestion : faces) {
            picker->faces.emplace_back(toRectF(localRectFromRegion(suggestion.region, picker->width, picker->height)),
                                       wideFromUtf8(suggestion.label));
        }
        // The scan is done for this display now: an empty list is the honest
        // "none found", no longer "not yet scanned".
        picker->facesScanned = true;
        if (picker->facesMode) {
            picker->suggestions = picker->faces;
            picker->hoveredSuggestion = -1;
        }
        paintPicker(*picker);

        return;
    }
}

void setRegionPickChipColor(const std::optional<FloatColor>& color)
{
    const bool colorChanged =
        color.has_value() != g_pinChipColor.has_value() || (color && g_pinChipColor &&
                                                            (std::lround(color->r) != std::lround(g_pinChipColor->r) ||
                                                             std::lround(color->g) != std::lround(g_pinChipColor->g) ||
                                                             std::lround(color->b) != std::lround(g_pinChipColor->b)));
    g_pinChipColor = color;
    if (g_pickers.empty() || !g_pickers.front()->pinMode) {
        return;
    }
    if (g_pinCursor && !colorChanged) {
        return;
    }
    if (!ensureGdiplus()) {
        return;
    }
    HCURSOR next = buildPinCursor(uiScale(g_pickers.front()->window), color);
    if (!next) {
        return;
    }
    HCURSOR previous = g_pinCursor;
    g_pinCursor = next;
    // Apply immediately while one of the overlays owns the mouse; the
    // next WM_SETCURSOR keeps it applied afterwards.
    POINT cursorPosition{};
    GetCursorPos(&cursorPosition);
    if (pickerForWindow(WindowFromPoint(cursorPosition)) || pickerForWindow(GetCapture())) {
        SetCursor(g_pinCursor);
    }
    if (previous) {
        DestroyIcon(previous);
    }
}

// Sizes, places, and repaints the border window for the current region.
// Directly beneath the scope window when one is visible: the border must
// never cover the scopes, and both live in the topmost band. The surface
// only shows size, scale, and entrance alpha; a plain move needs no
// repaint at all - the common case when the whole region is dragged.
void presentBorderWindow(double scale, const std::wstring& label)
{
    const auto pad = static_cast<int>(WindowPad * scale);
    HWND insertAfter = HWND_TOPMOST;
    const std::vector<HWND> own = ownWindows();
    if (!own.empty()) {
        insertAfter = own.front();
    }
    const int strip = static_cast<int>(LabelBand * scale);
    const int width = (g_border.region.right - g_border.region.left) + 2 * pad;
    const int height = (g_border.region.bottom - g_border.region.top) + 2 * pad + strip;
    // Paint before placing. The layered surface keeps its previous bitmap
    // until the repaint pushes a new one, anchored at the window's top-left;
    // placing first parks the outgoing border at the new corner for the
    // whole repaint - a visible ghost when the surface is display-sized.
    // The push carries position and size, so the placement below only
    // settles z-order and visibility.
    const bool repaint = width != g_border.paintedWidth || height != g_border.paintedHeight ||
                         scale != g_border.paintedScale || label != g_border.paintedLabel || g_border.alpha != 255;
    SS_DIAG(Border, "present pos=%ld,%ld size=%dx%d repaint=%d alpha=%d", g_border.region.left - pad,
            g_border.region.top - pad - strip, width, height, repaint ? 1 : 0, static_cast<int>(g_border.alpha));
    if (repaint) {
        paintBorder();
    }
    SetWindowPos(g_border.window, insertAfter, g_border.region.left - pad, g_border.region.top - pad - strip, width,
                 height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void showRegionBorder(uint32_t displayId, const RegionOfInterest& region, const std::string& attachedLabel,
                      bool attached)
{
    const auto geometry = geometryOfDisplay(displayId);
    if (!geometry) {
        return;
    }
    const std::wstring label = wideFromUtf8(attachedLabel.c_str());

    if (!g_border.window) {
        g_border.window = createOverlayWindow(L"SidescopesRegionBorder", borderProc,
                                              WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 0);
        if (!g_border.window) {
            return;
        }
    }

    RECT wanted{};
    wanted.left = static_cast<int>(geometry->originX + region.leftPercent / 100.0 * geometry->widthPoints);
    wanted.top = static_cast<int>(geometry->originY + region.topPercent / 100.0 * geometry->heightPoints);
    wanted.right = static_cast<int>(geometry->originX + region.rightPercent / 100.0 * geometry->widthPoints);
    wanted.bottom = static_cast<int>(geometry->originY + region.bottomPercent / 100.0 * geometry->heightPoints);
    // The host reconciles every frame; an unchanged border must cost
    // nothing. Mid-entrance the live rect lags the target, so the
    // comparison is against the target.
    const bool visible = IsWindowVisible(g_border.window) != FALSE;
    if (visible && EqualRect(&wanted, &g_border.appearTarget) && label == g_border.attachedLabel &&
        attached == g_border.attachedRegion) {
        return;
    }
    SS_DIAG(Border, "show wanted=%ld,%ld,%ld,%ld visible=%d appearing=%d", wanted.left, wanted.top, wanted.right,
            wanted.bottom, visible ? 1 : 0, g_border.appearing ? 1 : 0);

    g_border.displayOriginX = geometry->originX;
    g_border.displayOriginY = geometry->originY;
    g_border.displayWidth = geometry->widthPoints;
    g_border.displayHeight = geometry->heightPoints;
    g_border.region = wanted;
    g_border.appearTarget = wanted;
    g_border.attachedLabel = label;
    g_border.attachedRegion = attached;

    const double scale = uiScale(g_border.window);
    if (!visible) {
        beginBorderAppear(scale);
    } else if (g_border.appearing) {
        // The target moved mid-entrance: snap, never tween position.
        snapBorderAppear();
    }
    presentBorderWindow(scale, label);
}

void hideRegionBorder()
{
    if (g_border.window) {
        SS_DIAG(Border, "hide visible=%d appearing=%d", IsWindowVisible(g_border.window) ? 1 : 0,
                g_border.appearing ? 1 : 0);
        if (g_border.appearing) {
            snapBorderAppear();
        }
        ShowWindow(g_border.window, SW_HIDE);
        g_border.appearTarget = RECT{};
    }
}

namespace {

// The attached-edit spotlight: a click-through veil that dims everything
// outside the attached window while its border is dragged.
HWND g_editDimWindow = nullptr;
RECT g_editDimHole{};
uint32_t g_editDimDisplay = 0;

LRESULT CALLBACK editDimProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

void showAttachedEditDim(uint32_t displayId, const RegionOfInterest& windowRegion)
{
    const auto geometry = geometryOfDisplay(displayId);
    if (!geometry || !ensureGdiplus()) {
        return;
    }

    RECT hole{};
    hole.left = static_cast<int>(geometry->originX + windowRegion.leftPercent / 100.0 * geometry->widthPoints);
    hole.top = static_cast<int>(geometry->originY + windowRegion.topPercent / 100.0 * geometry->heightPoints);
    hole.right = static_cast<int>(geometry->originX + windowRegion.rightPercent / 100.0 * geometry->widthPoints);
    hole.bottom = static_cast<int>(geometry->originY + windowRegion.bottomPercent / 100.0 * geometry->heightPoints);
    if (g_editDimWindow && IsWindowVisible(g_editDimWindow) && EqualRect(&hole, &g_editDimHole) &&
        displayId == g_editDimDisplay) {
        return;
    }

    if (!g_editDimWindow) {
        // Mouse-transparent as well as no-activate: the veil informs, the
        // border and the editor keep every event.
        g_editDimWindow = createOverlayWindow(
            L"SidescopesEditDim", editDimProc,
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, 0);
        if (!g_editDimWindow) {
            return;
        }
    }
    g_editDimHole = hole;
    g_editDimDisplay = displayId;

    const int width = static_cast<int>(geometry->widthPoints);
    const int height = static_cast<int>(geometry->heightPoints);
    LayeredSurface surface(width, height);
    if (!surface.valid()) {
        return;
    }
    Gdiplus::Graphics canvas(surface.dc());
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush veil(Gdiplus::Color(115, 0, 0, 0));
    canvas.FillRectangle(&veil,
                         Gdiplus::RectF(0, 0, static_cast<Gdiplus::REAL>(width), static_cast<Gdiplus::REAL>(height)));
    const Gdiplus::RectF holeLocal(static_cast<Gdiplus::REAL>(hole.left - geometry->originX),
                                   static_cast<Gdiplus::REAL>(hole.top - geometry->originY),
                                   static_cast<Gdiplus::REAL>(hole.right - hole.left),
                                   static_cast<Gdiplus::REAL>(hole.bottom - hole.top));
    Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
    canvas.FillRectangle(&clear, holeLocal);
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    Gdiplus::Pen rim(Gdiplus::Color(230, 255, 214, 140), 1.5f);
    canvas.DrawRectangle(&rim, holeLocal);

    // Below the border in the topmost band: the veil must never cover the
    // handles.
    HWND insertAfter = g_border.window ? g_border.window : HWND_TOPMOST;
    SetWindowPos(g_editDimWindow, insertAfter, static_cast<int>(geometry->originX), static_cast<int>(geometry->originY),
                 width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    surface.push(g_editDimWindow, static_cast<int>(geometry->originX), static_cast<int>(geometry->originY));
}

void hideAttachedEditDim()
{
    if (g_editDimWindow) {
        ShowWindow(g_editDimWindow, SW_HIDE);
    }
}

std::vector<BorderKeyPress> drainBorderKeyPresses()
{
    // The Windows border never takes the keyboard (WS_EX_NOACTIVATE); the
    // editor keeps every key.
    return {};
}

RegionBorderEdit pollRegionBorderEdit()
{
    RegionBorderEdit edit;
    edit.editing = g_borderEditing;
    edit.dismissed = g_borderDismissed;
    g_borderDismissed = false;
    edit.attachToggled = g_borderAttachToggled;
    g_borderAttachToggled = false;
    if (g_borderEditChanged) {
        edit.region = g_borderEditRegion;
        g_borderEditChanged = false;
    }
    return edit;
}

}  // namespace sidescopes
