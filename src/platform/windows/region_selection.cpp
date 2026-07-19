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

#include "platform/icons.h"

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
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Windows 10 2004; absent from older SDKs.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#include "platform/desktop.h"
#include "platform/face_detection.h"
#include "platform/region_geometry.h"
#include "platform/windows/wide_strings.h"

namespace sidescopes {
namespace {

// The border window extends this far beyond the region: the grab band,
// wide enough to hit with a cursor, slim enough to stay unobtrusive.
constexpr double BorderPad = 12.0;
// The screenshot-style handle dots protrude past the band's outer edge;
// the window carries a transparent margin so they are not clipped.
constexpr double HandleRadius = 3.5;
// Thickness of the measured-edge ring between the region and the hazard
// stripes. The stripes' cutoff and the ring share this one constant, so
// they abut exactly at every display scale.
constexpr double EdgeRing = 1.0;
constexpr double HandleMargin = HandleRadius + 2.0;
constexpr double WindowPad = BorderPad + HandleMargin;
// Regions cannot shrink beyond this many points per side.
constexpr double MinimumRegionSize = 24.0;
// The hover-revealed close button: a badge on the band's outer corner,
// diagonally off the corner handle, so it visibly belongs to the region
// as a whole. Pulled inward a touch so the disc mostly rides the band;
// tiny regions still yield it to the resize zones.
// Extra window height above the band when the attached label is worn, so
// the name tab clears the handles instead of crowding the top-center one.
constexpr double LabelBand = 20.0;

/// The border's entrance: a short fade with a slight outward settle onto
/// its place, mirroring the macOS constants exactly. Hiding stays instant
/// - a stale border is wrong the moment it is stale. The settle inset is
/// absolute DIPs, capped for tiny regions, so the motion reads the same at
/// every region size.
constexpr double BorderAppearSeconds = 0.12;
constexpr double BorderSettlePoints = 16.0;
constexpr UINT_PTR BorderAppearTimer = 1;
constexpr double CloseRadius = 6.5;
constexpr double CloseHitRadius = 11.0;
constexpr double CloseCornerInset = 2.0;
constexpr double TabPinZone = 18.0;
constexpr double MinimumWidthForClose = 48.0;

bool ensureGdiplus()
{
    static ULONG_PTR token = 0;
    static bool ready = false;
    if (!ready) {
        Gdiplus::GdiplusStartupInput input;
        ready = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }
    return ready;
}

// All the point-sized constants scale with the window's monitor: Windows
// coordinates are physical pixels, and 12 of them is a very different
// distance at 100% and at 175%.
double uiScale(HWND window)
{
    const UINT dpi = GetDpiForWindow(window);
    return dpi > 0 ? dpi / 96.0 : 1.0;
}

// A layered window's backing store: a premultiplied 32-bit DIB wrapped in
// a GDI+ surface, pushed to the window with per-pixel alpha. Surfaces are
// cached across paints: at 4K the DIB is tens of megabytes, and both the
// allocation and the full-surface push are far too expensive to repeat on
// every mouse move.
class LayeredSurface
{
public:
    LayeredSurface(int width, int height)
        : m_width(width),
          m_height(height)
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // top-down rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        m_dc = CreateCompatibleDC(nullptr);
        m_bitmap = CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &m_bits, nullptr, 0);
        if (m_bitmap) {
            m_previous = SelectObject(m_dc, m_bitmap);
        }
    }

    ~LayeredSurface()
    {
        if (m_bitmap) {
            SelectObject(m_dc, m_previous);
            DeleteObject(m_bitmap);
        }
        if (m_dc) {
            DeleteDC(m_dc);
        }
    }

    LayeredSurface(const LayeredSurface&) = delete;
    LayeredSurface& operator=(const LayeredSurface&) = delete;

    [[nodiscard]] bool valid() const
    {
        return m_bitmap != nullptr;
    }

    [[nodiscard]] HDC dc() const
    {
        return m_dc;
    }

    [[nodiscard]] HBITMAP bitmapHandle() const
    {
        return m_bitmap;
    }

    [[nodiscard]] int width() const
    {
        return m_width;
    }

    [[nodiscard]] int height() const
    {
        return m_height;
    }

    void push(HWND window, int screenX, int screenY, BYTE alpha = 255)
    {
        POINT position{screenX, screenY};
        SIZE size{m_width, m_height};
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(window, nullptr, &position, &size, m_dc, &source, 0, &blend, ULW_ALPHA);
    }

    // Pushes only the given surface rectangle to the compositor. A drag
    // repaint touches a selection-sized sliver of a display-sized
    // surface; pushing all of it would upload the full bitmap each move.
    void pushDirty(HWND window, int screenX, int screenY, const Gdiplus::RectF& area)
    {
        RECT dirty{max(0, static_cast<int>(std::floor(area.X))), max(0, static_cast<int>(std::floor(area.Y))),
                   min(m_width, static_cast<int>(std::ceil(area.GetRight()))),
                   min(m_height, static_cast<int>(std::ceil(area.GetBottom())))};
        if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) {
            return;
        }
        POINT position{screenX, screenY};
        SIZE size{m_width, m_height};
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.pptDst = &position;
        info.psize = &size;
        info.hdcSrc = m_dc;
        info.pptSrc = &source;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = &dirty;
        UpdateLayeredWindowIndirect(window, &info);
    }

private:
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previous = nullptr;
    void* m_bits = nullptr;
    int m_width = 0;
    int m_height = 0;
};

// The overlay works in GDI+ RectF and the border in Win32 RECT; both adapt
// to the geometry layer's toolkit-independent LocalRect at the boundary.
LocalRect toLocalRect(const Gdiplus::RectF& rect)
{
    return {rect.X, rect.Y, rect.Width, rect.Height};
}

LocalRect toLocalRect(const RECT& rect)
{
    return {static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right - rect.left),
            static_cast<double>(rect.bottom - rect.top)};
}

Gdiplus::RectF toRectF(const LocalRect& rect)
{
    return {static_cast<Gdiplus::REAL>(rect.x), static_cast<Gdiplus::REAL>(rect.y),
            static_cast<Gdiplus::REAL>(rect.width), static_cast<Gdiplus::REAL>(rect.height)};
}

// ---------------------------------------------------------------------------
// Picker overlay
// ---------------------------------------------------------------------------

struct PickerState
{
    HWND window = nullptr;
    uint32_t displayId = 0;
    int originX = 0;  // the covered monitor, virtual-screen pixels
    int originY = 0;
    int width = 0;
    int height = 0;
    bool drawMode = false;
    bool facesMode = false;
    // The attached draw's clamp: set when a drag starts in window mode
    // over a suggestion - the drag cannot leave constraintRect
    // (overlay-local pixels), everything outside it dims hard, and the
    // label names the target window's application.
    bool constrained = false;
    Gdiplus::RectF constraintRect{};
    std::wstring constraintLabel;
    // A drag in window mode: draws an attached region within the window
    // under the drag's start instead of confirming a whole window.
    bool pickDragging = false;
    // Color pinning: a click reports a point to sample, a drag an area to
    // average, the region is never touched, and a cursor chip previews the
    // sample.
    bool pinMode = false;
    // The pending pin, in overlay-local pixels, left here until the next
    // poll collects it - with the click's Shift state, the per-pin choice
    // to keep picking. pinnedIsPoint says whether pinnedPoint (a click) or
    // pinnedArea (a drag) holds it.
    Gdiplus::PointF pinnedPoint{};
    Gdiplus::RectF pinnedArea{};
    bool pinnedIsPoint = false;
    bool pinnedKeepOpen = false;
    bool pinnedReady = false;
    // The active suggestion list in overlay-local pixels, with labels -
    // the windows or the faces, depending on the mode.
    std::vector<std::pair<Gdiplus::RectF, std::wstring>> windows;
    std::vector<std::pair<Gdiplus::RectF, std::wstring>> faces;
    std::vector<std::pair<Gdiplus::RectF, std::wstring>> suggestions;
    // This application's own windows, local pixels: they float above the
    // overlay, and the banner avoids sitting beneath them.
    std::vector<Gdiplus::RectF> exclusions;
    int hovered = -1;
    bool dragging = false;
    POINT dragStart{};
    POINT dragCurrent{};
    bool picked = false;
    bool finished = false;
    Gdiplus::RectF confirmed{};
    // The cached backing store, plus the selection rectangle it last
    // showed: successive drag repaints touch only the union of the two.
    std::unique_ptr<LayeredSurface> surface;
    Gdiplus::RectF paintedSelection{};
    bool selectionPainted = false;
};

// One overlay per display; a pick anywhere is a pick there. Mode flags
// live per overlay and are switched in lockstep, the way the shared
// keyboard expects.
std::vector<PickerState*> g_pickers;

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

HCURSOR g_pinCursor = nullptr;

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

PickerState* pickerForWindow(HWND window)
{
    for (PickerState* picker : g_pickers) {
        if (picker->window == window) {
            return picker;
        }
    }
    return nullptr;
}

struct BorderState
{
    HWND window = nullptr;
    // The display the region lives on, for percent conversions.
    double displayOriginX = 0.0;
    double displayOriginY = 0.0;
    double displayWidth = 0.0;
    double displayHeight = 0.0;
    // The region in virtual-screen pixels.
    RECT region{};
    unsigned dragZone = ZoneNone;
    POINT dragStartMouse{};
    RECT dragStartRegion{};
    bool closePressed = false;
    bool attachPressed = false;
    // Whether the outlined region is attached: picks the toggle's glyph.
    bool attachedRegion = false;
    // The cached backing store and the geometry it was painted for. The
    // band's look depends on the window's size and scale, never on its
    // position, so a move needs no repaint at all - the common case when
    // the whole region is dragged around.
    std::unique_ptr<LayeredSurface> surface;
    int paintedWidth = 0;
    int paintedHeight = 0;
    double paintedScale = 0.0;
    // Non-empty for a window-attached region: the tracked application's
    // name, worn above the band - the tell that this region belongs to a
    // window.
    std::wstring attachedLabel;
    std::wstring paintedLabel;
    // The entrance animation: the rect the border is heading to (region
    // lags it mid-flight), the start tick, and the whole-surface alpha.
    RECT appearTarget{};
    ULONGLONG appearStart = 0;
    bool appearing = false;
    BYTE alpha = 255;
};

BorderState g_border;

// Shared edit state the application polls once per frame.
bool g_borderEditing = false;
bool g_borderEditChanged = false;
bool g_borderDismissed = false;
bool g_borderAttachToggled = false;
RegionOfInterest g_borderEditRegion;

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
    const auto x = static_cast<Gdiplus::REAL>((picker.width - width) / 2);
    const auto topY = static_cast<Gdiplus::REAL>(80 * scale);
    const auto centerY = static_cast<Gdiplus::REAL>((picker.height - height) / 2);
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
// lift off the area being indicated.
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
            if (i == picker.hovered) {
                continue;
            }
            punchRect(canvas, picker.suggestions[static_cast<std::size_t>(i)].first);
            canvas.DrawRectangle(&outline, picker.suggestions[static_cast<std::size_t>(i)].first);
        }
    }
    if (picker.hovered >= 0 && picker.hovered < static_cast<int>(picker.suggestions.size())) {
        // Only the rectangle under the cursor is shown, washed with an
        // accent like window selection in the screenshot interfaces.
        const auto& hovered = picker.suggestions[static_cast<std::size_t>(picker.hovered)];
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
        drawBanner(canvas, picker, picker.suggestions.empty() ? L"No faces found on this screen" : L"Click a face",
                   L"[A] attach to a window    [D] draw    [Esc] full screen", picker.suggestions.empty(), scale);
    } else {
        drawBanner(canvas, picker, L"Click a window or drag an area inside it",
                   supportsFaceDetection() ? L"[F] pick a face    [D] draw    [Esc] full screen"
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
            secondary = L"[A] attach to a window    [F] pick a face    [Esc] full screen";
        } else if (!picker.windows.empty()) {
            secondary = L"[A] attach to a window    [Esc] full screen";
        }
        drawBanner(canvas, picker, L"Drag to select an area", secondary, false, scale);
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

// Repaints the overlay. With a dirty rectangle, only that area is redrawn
// and pushed - the clip keeps the rasterizer inside the changed sliver,
// which is what makes a display-sized layered window affordable to update
// per mouse move.
void paintPicker(PickerState& picker, const Gdiplus::RectF* dirty = nullptr)
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

// 0 = pick a window, 1 = draw, 2 = pick a face, 3 = pin colors. Face
// mode is offered even when no face was found: the honest answer is the
// empty overlay saying so, not a key that silently does nothing.
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
        picker->hovered = -1;
        picker->dragging = false;
        picker->pickDragging = false;
        picker->constrained = false;
        picker->selectionPainted = false;
        paintPicker(*picker);
    }
}

LRESULT pickerOnSetCursor(PickerState& picker)
{
    if (picker.pinMode) {
        SetCursor(g_pinCursor ? g_pinCursor : LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    }
    // Window mode draws as readily as it clicks; only the face pick is
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
    if (hovered != picker.hovered) {
        picker.hovered = hovered;
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
        picker.pinnedArea = selection;
        picker.pinnedIsPoint = false;
    } else {
        // A plain click pins the point itself, so the pinned color is
        // exactly what the live cursor readout showed; averaging a patch
        // here would fade pins taken over a small subject. Dragging an
        // area is the explicit way to average a swatch.
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
    picker.confirmed = picker.suggestions[static_cast<std::size_t>(hovered)].first;
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
            picker.confirmed = selection;
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

// ---------------------------------------------------------------------------
// Region border
// ---------------------------------------------------------------------------

// The hazard stripes as a repeating tile, rebuilt only when the scale
// changes. Painting the band as two hundred antialiased diagonal lines
// priced every repaint off the frame budget; one pre-rendered period
// fills the same band as a single textured rectangle. The pattern is
// periodic in both axes, and each stroke is overdrawn past every edge so
// the antialiased fringes wrap seamlessly.
struct StripeTile
{
    double scale = 0.0;
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
    std::unique_ptr<Gdiplus::TextureBrush> brush;
};

StripeTile g_stripeTile;

Gdiplus::TextureBrush* stripeBrushFor(double scale)
{
    if (g_stripeTile.brush && g_stripeTile.scale == scale) {
        return g_stripeTile.brush.get();
    }
    const int period = max(1, static_cast<int>(std::lround(10.0 * scale)));
    auto bitmap = std::make_unique<Gdiplus::Bitmap>(period, period, PixelFormat32bppPARGB);
    if (bitmap->GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    {
        Gdiplus::Graphics tile(bitmap.get());
        tile.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        tile.Clear(Gdiplus::Color(115, 26, 26, 26));
        Gdiplus::Pen stripePen(Gdiplus::Color(115, 230, 230, 230), static_cast<Gdiplus::REAL>(4.0 * scale));
        const auto p = static_cast<Gdiplus::REAL>(period);
        const auto overhang = static_cast<Gdiplus::REAL>(4.0 * scale);
        // Strokes of constant x+y, one period apart: the same diagonal
        // the full-band loop drew.
        for (int line = -1; line <= 2; ++line) {
            const auto c = static_cast<Gdiplus::REAL>(line) * p;
            tile.DrawLine(&stripePen, c + overhang, -overhang, c - p - overhang, p + overhang);
        }
    }
    g_stripeTile.scale = scale;
    g_stripeTile.bitmap = std::move(bitmap);
    g_stripeTile.brush = std::make_unique<Gdiplus::TextureBrush>(g_stripeTile.bitmap.get());
    g_stripeTile.brush->SetWrapMode(Gdiplus::WrapModeTile);
    return g_stripeTile.brush.get();
}

// The region rectangle in border-window-local pixels. The label strip, when
// worn, rides above the band and shifts the region down within the window.
Gdiplus::RectF borderRegionLocal(double scale)
{
    const auto pad = static_cast<Gdiplus::REAL>(WindowPad * scale);
    const auto strip = static_cast<Gdiplus::REAL>(LabelBand * scale);
    return {pad, pad + strip, static_cast<Gdiplus::REAL>(g_border.region.right - g_border.region.left),
            static_cast<Gdiplus::REAL>(g_border.region.bottom - g_border.region.top)};
}

// Eight handles, no modifier: the corners resize both axes, the edge
// midpoints resize their edge, and the rest of the band moves. The
// visible handles say which is which - a modifier key never could.
// Always visible while the border is up - hover-revealing it flickered
// on every band crossing, and crossing the band is what a cursor does
// all day. It still hides during drags and yields on tiny regions.
bool closeVisible(double scale)
{
    const Gdiplus::RectF region = borderRegionLocal(scale);
    return g_border.dragZone == ZoneNone && region.Width >= MinimumWidthForClose * scale;
}

// On the band's outer corner, at forty-five degrees off the top-right
// handle dot - anchored to the corner rather than parked beside it.
Gdiplus::PointF closeCenter(double scale)
{
    const Gdiplus::RectF region = borderRegionLocal(scale);
    return {static_cast<Gdiplus::REAL>(region.GetRight() + (BorderPad - CloseCornerInset) * scale),
            static_cast<Gdiplus::REAL>(region.Y - (BorderPad - CloseCornerInset + EdgeRing) * scale)};
}

// The attach toggle lives at the label tab's fixed left end: the same
// spot whatever the label says, clear of every handle.
Gdiplus::PointF attachButtonCenter(double scale)
{
    const Gdiplus::RectF region = borderRegionLocal(scale);
    return {static_cast<Gdiplus::REAL>(region.X + TabPinZone / 2 * scale),
            static_cast<Gdiplus::REAL>(region.Y - (WindowPad + LabelBand / 2) * scale)};
}

unsigned borderZoneAtPoint(double x, double y, double scale)
{
    const Gdiplus::RectF region = borderRegionLocal(scale);
    if (region.Contains(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y))) {
        return ZoneNone;  // click-through anyway
    }
    if (closeVisible(scale)) {
        const Gdiplus::PointF center = closeCenter(scale);
        const double dx = x - center.X;
        const double dy = y - center.Y;
        const double hit = CloseHitRadius * scale;
        if (dx * dx + dy * dy <= hit * hit) {
            return ZoneClose;
        }
    }
    {
        const Gdiplus::PointF center = attachButtonCenter(scale);
        const double dx = x - center.X;
        const double dy = y - center.Y;
        const double hit = CloseHitRadius * scale;
        if (dx * dx + dy * dy <= hit * hit) {
            return ZoneAttach;
        }
    }
    const LocalRect local = toLocalRect(region);
    const unsigned corner = cornerZoneAt(local, x, y, scale);
    if (corner != ZoneNone) {
        return corner;
    }
    return edgeOrMoveZoneAt(local, x, y, scale);
}

void applyBorderCursor(unsigned zone)
{
    if (zone == ZoneNone) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return;
    }
    if ((zone & (ZoneClose | ZoneAttach)) != 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return;
    }
    if ((zone & ZoneMove) != 0) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return;
    }
    const bool horizontal = (zone & (ZoneLeft | ZoneRight)) != 0;
    const bool vertical = (zone & (ZoneTop | ZoneBottom)) != 0;
    if (horizontal && vertical) {
        const bool falling = ((zone & ZoneLeft) != 0) == ((zone & ZoneTop) != 0);  // NW-SE diagonal
        SetCursor(LoadCursorW(nullptr, falling ? IDC_SIZENWSE : IDC_SIZENESW));
        return;
    }
    SetCursor(LoadCursorW(nullptr, horizontal ? IDC_SIZEWE : IDC_SIZENS));
}

// The whole grab band is muted hazard tape, its own light-dark
// alternation visible on any content. The interior is never painted
// and therefore stays click-through.
void paintBorderBand(Gdiplus::Graphics& canvas, const Gdiplus::RectF& region, double scale)
{
    const auto bandPad = static_cast<Gdiplus::REAL>(BorderPad * scale);
    const auto ring = static_cast<Gdiplus::REAL>(EdgeRing * scale);
    Gdiplus::RectF band(region.X - bandPad, region.Y - bandPad, region.Width + 2 * bandPad,
                        region.Height + 2 * bandPad);
    // The stripes stop short of the measured-edge ring: crossing it would
    // read as the band bleeding into the measured area.
    Gdiplus::RectF stripeHole(region.X - ring, region.Y - ring, region.Width + 2 * ring, region.Height + 2 * ring);
    canvas.SetClip(band);
    canvas.ExcludeClip(stripeHole);
    if (Gdiplus::TextureBrush* stripes = stripeBrushFor(scale)) {
        canvas.FillRectangle(stripes, band);
    } else {
        // Out of memory for a tile the size of a coin; the plain base
        // keeps the band visible.
        Gdiplus::SolidBrush bandBrush(Gdiplus::Color(115, 26, 26, 26));
        canvas.FillRectangle(&bandBrush, band);
    }
    canvas.ResetClip();
}

// The measured edge is a filled ring spanning exactly from the region
// to the stripes, with white dashes riding over the dark base so one
// of the two tones survives any background.
void paintBorderEdgeRing(Gdiplus::Graphics& canvas, const Gdiplus::RectF& region, double scale)
{
    const auto ring = static_cast<Gdiplus::REAL>(EdgeRing * scale);
    Gdiplus::RectF stripeHole(region.X - ring, region.Y - ring, region.Width + 2 * ring, region.Height + 2 * ring);
    canvas.SetClip(stripeHole);
    canvas.ExcludeClip(region);
    Gdiplus::SolidBrush ringBrush(Gdiplus::Color(217, 26, 26, 26));
    canvas.FillRectangle(&ringBrush, stripeHole);
    canvas.ResetClip();
    // Neutral greys only, both region kinds: any hue this close to the
    // sampled pixels would skew the eye's read of the photograph. The
    // label above the band is what tells an attached region apart.
    Gdiplus::Pen dashPen(Gdiplus::Color(242, 247, 247, 247), ring);
    const Gdiplus::REAL dashPattern[2] = {static_cast<Gdiplus::REAL>(4.0 * scale / (ring > 0 ? ring : 1)),
                                          static_cast<Gdiplus::REAL>(4.0 * scale / (ring > 0 ? ring : 1))};
    dashPen.SetDashPattern(dashPattern, 2);
    canvas.DrawRectangle(
        &dashPen, Gdiplus::RectF(region.X - ring / 2, region.Y - ring / 2, region.Width + ring, region.Height + ring));
}

// Eight handle dots - corners and edge midpoints - centered on the
// measurement line: small gray circles straddling the selection edge.
void paintBorderHandles(Gdiplus::Graphics& canvas, const Gdiplus::RectF& region, double scale)
{
    const auto radius = static_cast<Gdiplus::REAL>(HandleRadius * scale);
    const auto handle = [&](Gdiplus::REAL x, Gdiplus::REAL y) {
        const Gdiplus::RectF circle(x - radius, y - radius, radius * 2, radius * 2);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 199, 199, 199));
        canvas.FillEllipse(&fill, circle);
        // A dark rim beneath the near-white ring keeps the dot visible on
        // a bright sky; the ring matches the measurement line, so the dots
        // and the line read as one instrument.
        Gdiplus::Pen rim(Gdiplus::Color(179, 26, 26, 26), static_cast<Gdiplus::REAL>(2.0 * scale));
        canvas.DrawEllipse(&rim, circle);
        Gdiplus::Pen bright(Gdiplus::Color(242, 247, 247, 247), static_cast<Gdiplus::REAL>(1.0 * scale));
        canvas.DrawEllipse(&bright, circle);
    };
    handle(region.X, region.Y);
    handle(region.X + region.Width / 2, region.Y);
    handle(region.GetRight(), region.Y);
    handle(region.X, region.Y + region.Height / 2);
    handle(region.GetRight(), region.Y + region.Height / 2);
    handle(region.X, region.GetBottom());
    handle(region.X + region.Width / 2, region.GetBottom());
    handle(region.GetRight(), region.GetBottom());
}

// The hover-revealed close button, in the handles' own visual
// language: a dark disc where the dots are light, so it reads as an
// action rather than a grip, with the same bright ring and an x.
// The tracked window's name rides the band above the top edge: the attached
// region carries its own identification instead of the main window's toolbar
// doing it at a distance.
void paintBorderLabel(Gdiplus::Graphics& canvas, const Gdiplus::RectF& region, double scale)
{
    if (g_border.attachedLabel.empty()) {
        return;
    }
    // The tab hugs the text but never leaves the window: a title wider than
    // the region truncates at its tail, Preview-style, instead of being
    // clipped sharply at both ends by the surface bounds.
    Gdiplus::Font font(L"Segoe UI", static_cast<Gdiplus::REAL>(10.0 * scale));
    Gdiplus::RectF measured;
    canvas.MeasureString(g_border.attachedLabel.c_str(), -1, &font, Gdiplus::PointF(0, 0), &measured);
    const auto margin = static_cast<Gdiplus::REAL>(2.0 * scale);
    const auto padX = static_cast<Gdiplus::REAL>(6.0 * scale);
    const auto padY = static_cast<Gdiplus::REAL>(2.0 * scale);
    const auto pad = static_cast<Gdiplus::REAL>(WindowPad * scale);
    const Gdiplus::REAL surfaceWidth = region.Width + 2 * pad;
    const auto triangleZone = static_cast<Gdiplus::REAL>(TabPinZone * scale);
    // The tab holds the attach toggle at its fixed left end, then the
    // text; left-aligned with the region's own corner. The shared layout
    // keeps both platforms' degradation on small regions identical.
    const TabLayout layout =
        borderTabLayout(surfaceWidth - pad - margin, triangleZone, padX, measured.Width, 16.0 * scale);
    if (!layout.visible) {
        return;
    }
    const auto textWidth = static_cast<Gdiplus::REAL>(layout.textWidth);
    const auto tabWidth = static_cast<Gdiplus::REAL>(layout.tabWidth);
    const Gdiplus::REAL tabX = region.X;
    const Gdiplus::REAL centreY = region.Y - static_cast<Gdiplus::REAL>((WindowPad + LabelBand / 2) * scale);
    const Gdiplus::RectF tab(tabX, centreY - measured.Height / 2 - padY, tabWidth, measured.Height + 2 * padY);
    Gdiplus::SolidBrush plate(Gdiplus::Color(217, 26, 26, 26));
    canvas.FillRectangle(&plate, tab);
    Gdiplus::Pen rim(Gdiplus::Color(153, 190, 190, 190), static_cast<Gdiplus::REAL>(1.0 * scale));
    canvas.DrawRectangle(&rim, tab);
    Gdiplus::StringFormat format;
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::SolidBrush ink(Gdiplus::Color(242, 247, 247, 247));
    const Gdiplus::RectF textRect(tab.X + triangleZone + padX, tab.Y + padY, textWidth, measured.Height);
    canvas.DrawString(g_border.attachedLabel.c_str(), -1, &font, textRect, &format, &ink);
}

void paintBorderCloseButton(Gdiplus::Graphics& canvas, double scale)
{
    if (!closeVisible(scale)) {
        return;
    }
    const Gdiplus::PointF center = closeCenter(scale);
    const auto closeRadius = static_cast<Gdiplus::REAL>(CloseRadius * scale);
    const Gdiplus::RectF disc(center.X - closeRadius, center.Y - closeRadius, closeRadius * 2, closeRadius * 2);
    Gdiplus::SolidBrush discBrush(Gdiplus::Color(217, 26, 26, 26));
    canvas.FillEllipse(&discBrush, disc);
    Gdiplus::Pen discRing(Gdiplus::Color(242, 247, 247, 247), static_cast<Gdiplus::REAL>(1.0 * scale));
    canvas.DrawEllipse(&discRing, disc);
    const auto arm = static_cast<Gdiplus::REAL>((CloseRadius - 3.7) * scale);
    Gdiplus::Pen cross(Gdiplus::Color(242, 247, 247, 247), static_cast<Gdiplus::REAL>(1.3 * scale));
    cross.SetStartCap(Gdiplus::LineCapRound);
    cross.SetEndCap(Gdiplus::LineCapRound);
    canvas.DrawLine(&cross, center.X - arm, center.Y - arm, center.X + arm, center.Y + arm);
    canvas.DrawLine(&cross, center.X - arm, center.Y + arm, center.X + arm, center.Y - arm);
}

// The icon set's pushpin at the tab's fixed left end, showing the
// STATE: the pin while the region is attached, the struck-through pin
// while it is global. Rasterized once from the embedded vector sources,
// so every platform draws the identical icons.
void paintBorderAttachButton(Gdiplus::Graphics& canvas, double scale)
{
    const Gdiplus::PointF center = attachButtonCenter(scale);
    static std::unique_ptr<Gdiplus::Bitmap> icons[2];
    static int iconSize = 0;
    const int which = g_border.attachedRegion ? 1 : 0;
    const int pixels = std::max(8, static_cast<int>(std::lround(11.0 * scale)));
    if (!icons[which] || iconSize != pixels) {
        if (iconSize != pixels) {
            icons[0].reset();
            icons[1].reset();
        }
        // GDI+'s 32bppARGB is BGRA in memory: swap channels on the copy.
        const std::vector<uint8_t> rgba = rasterizeIcon(which == 1 ? Icon::Pin : Icon::PinOff, pixels);
        auto bitmap = std::make_unique<Gdiplus::Bitmap>(pixels, pixels, PixelFormat32bppARGB);
        Gdiplus::BitmapData data{};
        const Gdiplus::Rect lock(0, 0, pixels, pixels);
        if (bitmap->LockBits(&lock, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
            for (int y = 0; y < pixels; ++y) {
                auto* row = static_cast<uint8_t*>(data.Scan0) + static_cast<std::size_t>(y) * data.Stride;
                const uint8_t* source = rgba.data() + static_cast<std::size_t>(y) * pixels * 4;
                for (int x = 0; x < pixels; ++x) {
                    row[x * 4 + 0] = source[x * 4 + 2];
                    row[x * 4 + 1] = source[x * 4 + 1];
                    row[x * 4 + 2] = source[x * 4 + 0];
                    row[x * 4 + 3] = source[x * 4 + 3];
                }
            }
            bitmap->UnlockBits(&data);
        }
        icons[which] = std::move(bitmap);
        iconSize = pixels;
    }
    canvas.DrawImage(icons[which].get(),
                     Gdiplus::RectF(center.X - static_cast<Gdiplus::REAL>(pixels) / 2,
                                    center.Y - static_cast<Gdiplus::REAL>(pixels) / 2,
                                    static_cast<Gdiplus::REAL>(pixels), static_cast<Gdiplus::REAL>(pixels)));
}

void paintBorder()
{
    if (!g_border.window || !ensureGdiplus()) {
        return;
    }
    const double scale = uiScale(g_border.window);
    const auto pad = static_cast<Gdiplus::REAL>(WindowPad * scale);
    const auto strip = static_cast<int>(LabelBand * scale);
    const Gdiplus::RectF region = borderRegionLocal(scale);
    const int width = static_cast<int>(region.Width + 2 * pad);
    const int height = static_cast<int>(region.Height + 2 * pad) + strip;
    if (!g_border.surface || g_border.surface->width() != width || g_border.surface->height() != height) {
        g_border.surface = std::make_unique<LayeredSurface>(width, height);
    }
    LayeredSurface& surface = *g_border.surface;
    if (!surface.valid()) {
        return;
    }
    g_border.paintedWidth = width;
    g_border.paintedHeight = height;
    g_border.paintedScale = scale;
    Gdiplus::Graphics canvas(surface.dc());
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush erase(Gdiplus::Color(0, 0, 0, 0));
    canvas.FillRectangle(&erase,
                         Gdiplus::RectF(0, 0, static_cast<Gdiplus::REAL>(width), static_cast<Gdiplus::REAL>(height)));
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    paintBorderBand(canvas, region, scale);
    paintBorderEdgeRing(canvas, region, scale);
    paintBorderHandles(canvas, region, scale);
    paintBorderLabel(canvas, region, scale);
    paintBorderCloseButton(canvas, scale);
    paintBorderAttachButton(canvas, scale);
    g_border.paintedLabel = g_border.attachedLabel;

    surface.push(g_border.window, g_border.region.left - static_cast<int>(WindowPad * scale),
                 g_border.region.top - static_cast<int>(WindowPad * scale) - strip, g_border.alpha);
}

int borderSettleInset(const RECT& target, double scale)
{
    return static_cast<int>(
        std::min({BorderSettlePoints * scale, (target.right - target.left) / 6.0, (target.bottom - target.top) / 6.0}));
}

void beginBorderAppear(double scale)
{
    g_border.appearing = true;
    g_border.appearStart = GetTickCount64();
    const int inset = borderSettleInset(g_border.appearTarget, scale);
    InflateRect(&g_border.region, -inset, -inset);
    g_border.alpha = 0;
    SetTimer(g_border.window, BorderAppearTimer, 15, nullptr);
}

void snapBorderAppear()
{
    KillTimer(g_border.window, BorderAppearTimer);
    g_border.appearing = false;
    g_border.alpha = 255;
}

// One tick of the entrance: eased alpha and outward settle, the same
// curve and constants as the macOS side.
void advanceBorderAppear()
{
    const double elapsed = static_cast<double>(GetTickCount64() - g_border.appearStart) / 1000.0;
    const double t = std::min(1.0, elapsed / BorderAppearSeconds);
    const double eased = 1.0 - (1.0 - t) * (1.0 - t);
    const double scale = uiScale(g_border.window);
    const int inset = static_cast<int>(borderSettleInset(g_border.appearTarget, scale) * (1.0 - eased));
    g_border.region = g_border.appearTarget;
    InflateRect(&g_border.region, -inset, -inset);
    g_border.alpha = static_cast<BYTE>(eased * 255.0);
    if (t >= 1.0) {
        g_border.region = g_border.appearTarget;
        snapBorderAppear();
    }
    const auto pad = static_cast<int>(WindowPad * scale);
    const int strip = static_cast<int>(LabelBand * scale);
    const int width = (g_border.region.right - g_border.region.left) + 2 * pad;
    const int height = (g_border.region.bottom - g_border.region.top) + 2 * pad + strip;
    SetWindowPos(g_border.window, nullptr, g_border.region.left - pad, g_border.region.top - pad - strip, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    paintBorder();
}

// The interior is the editor's, not ours; only the band takes the mouse.
LRESULT borderHitTest(HWND window, LPARAM lParam)
{
    const double scale = uiScale(window);
    RECT frame{};
    GetWindowRect(window, &frame);
    const double x = static_cast<short>(LOWORD(lParam)) - frame.left;
    const double y = static_cast<short>(HIWORD(lParam)) - frame.top;
    return borderZoneAtPoint(x, y, scale) == ZoneNone ? HTTRANSPARENT : HTCLIENT;
}

LRESULT borderUpdateCursor(HWND window)
{
    if (g_border.dragZone != ZoneNone) {
        applyBorderCursor(g_border.dragZone);
        return TRUE;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    RECT frame{};
    GetWindowRect(window, &frame);
    const double scale = uiScale(window);
    applyBorderCursor(borderZoneAtPoint(cursor.x - frame.left, cursor.y - frame.top, scale));
    return TRUE;
}

LRESULT borderOnLButtonDown(HWND window, LPARAM lParam)
{
    const double scale = uiScale(window);
    const double x = static_cast<short>(LOWORD(lParam));
    const double y = static_cast<short>(HIWORD(lParam));
    const unsigned zone = borderZoneAtPoint(x, y, scale);
    if (zone == ZoneNone) {
        return 0;
    }
    if ((zone & ZoneClose) != 0) {
        g_border.closePressed = true;
        return 0;
    }
    if ((zone & ZoneAttach) != 0) {
        g_border.attachPressed = true;
        return 0;
    }
    g_border.dragZone = zone;
    GetCursorPos(&g_border.dragStartMouse);
    g_border.dragStartRegion = g_border.region;
    g_borderEditing = true;
    SetCapture(window);
    paintBorder();  // the close button hides while dragging
    return 0;
}

LRESULT borderOnMouseMove(HWND window)
{
    if (g_border.dragZone == ZoneNone) {
        return 0;
    }
    // Screen coordinates throughout: the window itself moves as
    // the application applies each edit, so client coordinates
    // shift under the cursor mid-drag.
    POINT mouse{};
    GetCursorPos(&mouse);
    const int dx = mouse.x - g_border.dragStartMouse.x;
    const int dy = mouse.y - g_border.dragStartMouse.y;
    const double scale = uiScale(window);
    // Truncated to whole pixels as the integer border rectangle always was,
    // so the clamp lands exactly where it did before.
    const int minimum = static_cast<int>(MinimumRegionSize * scale);
    const LocalRect dragged =
        draggedRegionRect(g_border.dragZone, toLocalRect(g_border.dragStartRegion), dx, dy, minimum);
    RegionOfInterest region;
    region.leftPercent = (dragged.x - g_border.displayOriginX) / g_border.displayWidth * 100.0;
    region.topPercent = (dragged.y - g_border.displayOriginY) / g_border.displayHeight * 100.0;
    region.rightPercent = (dragged.x + dragged.width - g_border.displayOriginX) / g_border.displayWidth * 100.0;
    region.bottomPercent = (dragged.y + dragged.height - g_border.displayOriginY) / g_border.displayHeight * 100.0;
    g_borderEditRegion = region;
    g_borderEditChanged = true;
    return 0;
}

LRESULT borderOnLButtonUp(HWND window, LPARAM lParam)
{
    if (g_border.closePressed) {
        g_border.closePressed = false;
        const double scale = uiScale(window);
        const double x = static_cast<short>(LOWORD(lParam));
        const double y = static_cast<short>(HIWORD(lParam));
        if ((borderZoneAtPoint(x, y, scale) & ZoneClose) != 0) {
            g_borderDismissed = true;
        }
        return 0;
    }
    if (g_border.attachPressed) {
        g_border.attachPressed = false;
        const double scale = uiScale(window);
        const double x = static_cast<short>(LOWORD(lParam));
        const double y = static_cast<short>(HIWORD(lParam));
        if ((borderZoneAtPoint(x, y, scale) & ZoneAttach) != 0) {
            g_borderAttachToggled = true;
        }
        return 0;
    }
    if (g_border.dragZone != ZoneNone) {
        g_border.dragZone = ZoneNone;
        g_borderEditing = false;
        ReleaseCapture();
        paintBorder();
    }
    return 0;
}

LRESULT CALLBACK borderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_MOUSEACTIVATE:
        // The band is mouse-only. WS_EX_NOACTIVATE keeps it from
        // BECOMING the foreground window, but without this answer a
        // click still DEACTIVATES whichever window holds the
        // keyboard - every shortcut dead until the scope window is
        // clicked again, with the band taking nothing in exchange.
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return borderHitTest(window, lParam);
    case WM_SETCURSOR:
        return borderUpdateCursor(window);
    case WM_LBUTTONDOWN:
        return borderOnLButtonDown(window, lParam);
    case WM_MOUSEMOVE:
        return borderOnMouseMove(window);
    case WM_LBUTTONUP:
        return borderOnLButtonUp(window, lParam);
    case WM_TIMER:
        if (wParam == BorderAppearTimer) {
            advanceBorderAppear();
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
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
    // application-level exclusion, so each window excludes itself. Best
    // effort: unsupported before Windows 10 2004.
    if (window) {
        if (!captureExclusionDisabled()) {
            SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
        }
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
void collectPinnedArea(RegionPickPoll& poll)
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
            poll.pinnedArea = regionFromLocalRect(toLocalRect(picker->pinnedArea), picker->width, picker->height);
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
        poll.confirmed =
            regionFromLocalRect(toLocalRect(finishedPicker->confirmed), finishedPicker->width, finishedPicker->height);
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
            if (picker->hovered >= 0 && picker->hovered < static_cast<int>(picker->suggestions.size())) {
                poll.preview = regionFromLocalRect(
                    toLocalRect(picker->suggestions[static_cast<std::size_t>(picker->hovered)].first), picker->width,
                    picker->height);
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

    // A window pick with nothing to pick anywhere opens as drawing, like
    // before, but the decision is global so every display shows the same
    // mode.
    bool anyWindows = false;
    for (const PickerDisplay& entry : displays) {
        anyWindows |= !entry.windows.empty();
    }
    const bool pin = initialMode == RegionPickerMode::PinColor;
    const bool draw = !pin && (initialMode == RegionPickerMode::Draw ||
                               (initialMode == RegionPickerMode::PickWindows && !anyWindows));
    const bool faces = initialMode == RegionPickerMode::PickFaces;

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
    poll.windowMode = !g_pickers.front()->pinMode && !g_pickers.front()->drawMode && !g_pickers.front()->facesMode;
    collectPinnedArea(poll);
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
    switchPickerMode(mode == RegionPickerMode::Draw        ? 1
                     : mode == RegionPickerMode::PickFaces ? 2
                     : mode == RegionPickerMode::PinColor  ? 3
                                                           : 0);
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
    SetWindowPos(g_border.window, insertAfter, g_border.region.left - pad, g_border.region.top - pad - strip, width,
                 height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (width != g_border.paintedWidth || height != g_border.paintedHeight || scale != g_border.paintedScale ||
        label != g_border.paintedLabel || g_border.alpha != 255) {
        paintBorder();
    }
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
        if (g_border.appearing) {
            snapBorderAppear();
        }
        ShowWindow(g_border.window, SW_HIDE);
        g_border.appearTarget = RECT{};
    }
}

namespace {

// The attached-edit spotlight: a click-through veil that dims everything
// outside the tracked window while its border is dragged.
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
