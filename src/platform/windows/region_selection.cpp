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
#include "platform/windows/wide_strings.h"

namespace sidescopes {
namespace {

// The border window extends this far beyond the region: the grab band,
// wide enough to hit with a cursor, slim enough to stay unobtrusive.
constexpr double kBorderPad = 12.0;
// The screenshot-style handle dots protrude past the band's outer edge;
// the window carries a transparent margin so they are not clipped.
constexpr double kHandleRadius = 3.5;
// Thickness of the measured-edge ring between the region and the hazard
// stripes. The stripes' cutoff and the ring share this one constant, so
// they abut exactly at every display scale.
constexpr double kEdgeRing = 1.0;
constexpr double kHandleMargin = kHandleRadius + 2.0;
constexpr double kWindowPad = kBorderPad + kHandleMargin;
// Within this distance of a region corner, a grab resizes both axes.
constexpr double kCornerZone = 22.0;
// Half-length of the edge-midpoint handle's grab zone along its edge.
constexpr double kMidpointZone = 22.0;
// Regions cannot shrink beyond this many points per side.
constexpr double kMinimumRegionSize = 24.0;
// The hover-revealed close button: a badge on the band's outer corner,
// diagonally off the corner handle, so it visibly belongs to the region
// as a whole. Pulled inward a touch so the disc mostly rides the band;
// tiny regions still yield it to the resize zones.
constexpr double kCloseRadius = 6.5;
constexpr double kCloseHitRadius = 11.0;
constexpr double kCloseCornerInset = 2.0;
constexpr double kMinimumWidthForClose = 48.0;

// Which edges a border drag adjusts; Move relocates the whole region.
enum ZoneBits : unsigned {
    kZoneNone = 0,
    kZoneLeft = 1u << 0,
    kZoneRight = 1u << 1,
    kZoneTop = 1u << 2,
    kZoneBottom = 1u << 3,
    kZoneMove = 1u << 4,
    kZoneClose = 1u << 5,
};

bool EnsureGdiplus() {
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
double UiScale(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return dpi > 0 ? dpi / 96.0 : 1.0;
}

// A layered window's backing store: a premultiplied 32-bit DIB wrapped in
// a GDI+ surface, pushed to the window with per-pixel alpha. Surfaces are
// cached across paints: at 4K the DIB is tens of megabytes, and both the
// allocation and the full-surface push are far too expensive to repeat on
// every mouse move.
class LayeredSurface {
public:
    LayeredSurface(int width, int height) : width_(width), height_(height) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // top-down rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(nullptr);
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (bitmap_) previous_ = SelectObject(dc_, bitmap_);
    }

    ~LayeredSurface() {
        if (bitmap_) {
            SelectObject(dc_, previous_);
            DeleteObject(bitmap_);
        }
        if (dc_) DeleteDC(dc_);
    }

    LayeredSurface(const LayeredSurface&) = delete;
    LayeredSurface& operator=(const LayeredSurface&) = delete;

    [[nodiscard]] bool Valid() const { return bitmap_ != nullptr; }
    [[nodiscard]] HDC Dc() const { return dc_; }
    [[nodiscard]] HBITMAP BitmapHandle() const { return bitmap_; }
    [[nodiscard]] int Width() const { return width_; }
    [[nodiscard]] int Height() const { return height_; }

    void Push(HWND window, int screen_x, int screen_y) {
        POINT position{screen_x, screen_y};
        SIZE size{width_, height_};
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(window, nullptr, &position, &size, dc_, &source, 0, &blend, ULW_ALPHA);
    }

    // Pushes only the given surface rectangle to the compositor. A drag
    // repaint touches a selection-sized sliver of a display-sized
    // surface; pushing all of it would upload the full bitmap each move.
    void PushDirty(HWND window, int screen_x, int screen_y, const Gdiplus::RectF& area) {
        RECT dirty{max(0, static_cast<int>(std::floor(area.X))),
                   max(0, static_cast<int>(std::floor(area.Y))),
                   min(width_, static_cast<int>(std::ceil(area.GetRight()))),
                   min(height_, static_cast<int>(std::ceil(area.GetBottom())))};
        if (dirty.right <= dirty.left || dirty.bottom <= dirty.top) return;
        POINT position{screen_x, screen_y};
        SIZE size{width_, height_};
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UPDATELAYEREDWINDOWINFO info{};
        info.cbSize = sizeof(info);
        info.pptDst = &position;
        info.psize = &size;
        info.hdcSrc = dc_;
        info.pptSrc = &source;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = &dirty;
        UpdateLayeredWindowIndirect(window, &info);
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

RegionOfInterest RegionFromLocalRect(const Gdiplus::RectF& rect, double width, double height) {
    RegionOfInterest region;
    region.left_percent = rect.X / width * 100.0;
    region.top_percent = rect.Y / height * 100.0;
    region.right_percent = (rect.X + rect.Width) / width * 100.0;
    region.bottom_percent = (rect.Y + rect.Height) / height * 100.0;
    return region;
}

Gdiplus::RectF LocalRectFromRegion(const RegionOfInterest& region, double width, double height) {
    return {
        static_cast<Gdiplus::REAL>(region.left_percent / 100.0 * width),
        static_cast<Gdiplus::REAL>(region.top_percent / 100.0 * height),
        static_cast<Gdiplus::REAL>((region.right_percent - region.left_percent) / 100.0 * width),
        static_cast<Gdiplus::REAL>((region.bottom_percent - region.top_percent) / 100.0 * height)};
}

// ---------------------------------------------------------------------------
// Picker overlay
// ---------------------------------------------------------------------------

struct PickerState {
    HWND window = nullptr;
    uint32_t display_id = 0;
    int origin_x = 0;  // the covered monitor, virtual-screen pixels
    int origin_y = 0;
    int width = 0;
    int height = 0;
    bool draw_mode = false;
    bool faces_mode = false;
    // Color pinning: clicks and drags report areas to average, the
    // region is never touched, and a cursor chip previews the sample.
    bool pin_mode = false;
    // An area the application should average and pin, in overlay-local
    // pixels, left here until the next poll collects it - with the
    // click's Shift state, the per-pin choice to keep picking.
    Gdiplus::RectF pinned_area{};
    bool pinned_keep_open = false;
    bool pinned_ready = false;
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
    POINT drag_start{};
    POINT drag_current{};
    bool picked = false;
    bool finished = false;
    Gdiplus::RectF confirmed{};
    // The cached backing store, plus the selection rectangle it last
    // showed: successive drag repaints touch only the union of the two.
    std::unique_ptr<LayeredSurface> surface;
    Gdiplus::RectF painted_selection{};
    bool selection_painted = false;
};

// One overlay per display; a pick anywhere is a pick there. Mode flags
// live per overlay and are switched in lockstep, the way the shared
// keyboard expects.
std::vector<PickerState*> g_pickers;

// The pin cursor's swatch color, pushed by the application once per
// frame; sampled from the capture stream so the swatch previews exactly
// what a click would pin.
std::optional<FloatColor> g_pin_chip_color;

// The sample patch a pin-mode click averages, in interface points.
constexpr double kPinSamplePoints = 14.0;

// The pin cursor: crosshair and preview swatch drawn into the CURSOR
// itself. A swatch painted into the overlay always trails the hardware
// cursor by a composition frame - the mouse image rides its own
// zero-latency plane, so the swatch does too. The crosshair is a
// two-tone grey, the look the system crosshair only has over dimmed
// content.
constexpr double kPinCursorHotspot = 12.0;  // crosshair center, points
constexpr double kPinCursorArm = 8.0;
constexpr double kPinCursorGap = 2.0;
constexpr double kPinSwatchOffset = 7.0;  // from the hotspot, points
constexpr double kPinSwatchSize = 13.0;

HCURSOR g_pin_cursor = nullptr;

HCURSOR BuildPinCursor(double scale, const std::optional<FloatColor>& color) {
    const int side =
        static_cast<int>((kPinCursorHotspot + kPinSwatchOffset + kPinSwatchSize) * scale + 4);
    const auto hotspot = static_cast<int>(kPinCursorHotspot * scale);
    LayeredSurface surface(side, side);
    if (!surface.Valid()) return nullptr;
    Gdiplus::Graphics canvas(surface.Dc());
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const auto center = static_cast<Gdiplus::REAL>(hotspot);
    const auto arm = static_cast<Gdiplus::REAL>(kPinCursorArm * scale);
    const auto gap = static_cast<Gdiplus::REAL>(kPinCursorGap * scale);
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
        const auto offset = static_cast<Gdiplus::REAL>(kPinSwatchOffset * scale);
        const auto size = static_cast<Gdiplus::REAL>(kPinSwatchSize * scale);
        const Gdiplus::RectF swatch(center + offset, center + offset, size, size);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, static_cast<BYTE>(color->r),
                                                static_cast<BYTE>(color->g),
                                                static_cast<BYTE>(color->b)));
        canvas.FillRectangle(&fill, swatch);
        Gdiplus::Pen rim(Gdiplus::Color(179, 26, 26, 26), static_cast<Gdiplus::REAL>(2.0 * scale));
        canvas.DrawRectangle(&rim, swatch);
        Gdiplus::Pen ring(Gdiplus::Color(242, 247, 247, 247),
                          static_cast<Gdiplus::REAL>(1.0 * scale));
        canvas.DrawRectangle(&ring, swatch);
    }

    // CreateIconIndirect copies both bitmaps; the surface and the mask
    // are ours to free.
    HBITMAP mask = CreateBitmap(side, side, 1, 1, nullptr);
    if (!mask) return nullptr;
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(hotspot);
    info.yHotspot = static_cast<DWORD>(hotspot);
    info.hbmMask = mask;
    info.hbmColor = surface.BitmapHandle();
    HCURSOR cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(mask);
    return cursor;
}

PickerState* PickerForWindow(HWND window) {
    for (PickerState* picker : g_pickers) {
        if (picker->window == window) return picker;
    }
    return nullptr;
}

struct BorderState {
    HWND window = nullptr;
    // The display the region lives on, for percent conversions.
    double display_origin_x = 0.0;
    double display_origin_y = 0.0;
    double display_width = 0.0;
    double display_height = 0.0;
    // The region in virtual-screen pixels.
    RECT region{};
    unsigned drag_zone = kZoneNone;
    POINT drag_start_mouse{};
    RECT drag_start_region{};
    bool close_pressed = false;
    // The cached backing store and the geometry it was painted for. The
    // band's look depends on the window's size and scale, never on its
    // position, so a move needs no repaint at all - the common case when
    // the whole region is dragged around.
    std::unique_ptr<LayeredSurface> surface;
    int painted_width = 0;
    int painted_height = 0;
    double painted_scale = 0.0;
};

BorderState g_border;

// Shared edit state the application polls once per frame.
bool g_border_editing = false;
bool g_border_edit_changed = false;
bool g_border_dismissed = false;
RegionOfInterest g_border_edit_region;

// This application's own top-level windows, except the overlays
// themselves.
std::vector<HWND> OwnWindows() {
    struct Collector {
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
            if (process != state->process || !IsWindowVisible(window)) return TRUE;
            if (PickerForWindow(window) != nullptr) return TRUE;
            if (window == g_border.window) return TRUE;
            state->windows->push_back(window);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));
    return windows;
}

std::vector<Gdiplus::RectF> OwnWindowExclusions(const PickerState& picker) {
    std::vector<Gdiplus::RectF> exclusions;
    for (HWND window : OwnWindows()) {
        RECT rect{};
        if (!GetWindowRect(window, &rect)) continue;
        exclusions.emplace_back(static_cast<Gdiplus::REAL>(rect.left - picker.origin_x),
                                static_cast<Gdiplus::REAL>(rect.top - picker.origin_y),
                                static_cast<Gdiplus::REAL>(rect.right - rect.left),
                                static_cast<Gdiplus::REAL>(rect.bottom - rect.top));
    }
    return exclusions;
}

Gdiplus::RectF SelectionRect(const PickerState& picker) {
    const auto left =
        static_cast<Gdiplus::REAL>(std::min(picker.drag_start.x, picker.drag_current.x));
    const auto top =
        static_cast<Gdiplus::REAL>(std::min(picker.drag_start.y, picker.drag_current.y));
    const auto width =
        static_cast<Gdiplus::REAL>(std::abs(picker.drag_current.x - picker.drag_start.x));
    const auto height =
        static_cast<Gdiplus::REAL>(std::abs(picker.drag_current.y - picker.drag_start.y));
    return {left, top, width, height};
}

// The smallest suggestion under the cursor wins, so a photo canvas beats
// the window that contains it.
int SuggestionAtPoint(const PickerState& picker, POINT point) {
    int best = -1;
    float best_area = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < picker.suggestions.size(); ++index) {
        const Gdiplus::RectF& rect = picker.suggestions[index].first;
        if (!rect.Contains(static_cast<Gdiplus::REAL>(point.x),
                           static_cast<Gdiplus::REAL>(point.y)))
            continue;
        const float area = rect.Width * rect.Height;
        if (area < best_area) {
            best_area = area;
            best = static_cast<int>(index);
        }
    }
    return best;
}

// The mode instruction: a primary line and a bracketed-keys line on a
// dark pill, placed where this application's own windows do not cover it
// - they float above the overlay, and a message half-hidden behind the
// scope window reads as a glitch.
void DrawBanner(Gdiplus::Graphics& canvas, const PickerState& picker, const wchar_t* primary,
                const wchar_t* secondary, bool prefer_center, double scale) {
    const Gdiplus::FontFamily family(L"Segoe UI");
    const Gdiplus::Font primary_font(&family, static_cast<Gdiplus::REAL>(22 * scale),
                                     Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    const Gdiplus::Font secondary_font(&family, static_cast<Gdiplus::REAL>(14 * scale),
                                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::RectF primary_size;
    Gdiplus::RectF secondary_size;
    canvas.MeasureString(primary, -1, &primary_font, Gdiplus::PointF(0, 0), &primary_size);
    canvas.MeasureString(secondary, -1, &secondary_font, Gdiplus::PointF(0, 0), &secondary_size);

    const auto width =
        static_cast<Gdiplus::REAL>(std::max(primary_size.Width, secondary_size.Width) + 48 * scale);
    const auto height =
        static_cast<Gdiplus::REAL>(primary_size.Height + secondary_size.Height + 30 * scale);
    const auto x = static_cast<Gdiplus::REAL>((picker.width - width) / 2);
    const auto top_y = static_cast<Gdiplus::REAL>(80 * scale);
    const auto center_y = static_cast<Gdiplus::REAL>((picker.height - height) / 2);
    const auto low_y = static_cast<Gdiplus::REAL>(picker.height * 0.78 - height);
    const Gdiplus::REAL candidates[3] = {prefer_center ? center_y : top_y,
                                         prefer_center ? top_y : center_y, low_y};
    Gdiplus::RectF banner(x, candidates[0], width, height);
    const auto margin = static_cast<Gdiplus::REAL>(12 * scale);
    for (const Gdiplus::REAL candidate : candidates) {
        Gdiplus::RectF probe(x - margin, candidate - margin, width + 2 * margin,
                             height + 2 * margin);
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
    pill.AddArc(banner.GetRight() - radius * 2, banner.GetBottom() - radius * 2, radius * 2,
                radius * 2, 0, 90);
    pill.AddArc(banner.X, banner.GetBottom() - radius * 2, radius * 2, radius * 2, 90, 90);
    pill.CloseFigure();
    Gdiplus::SolidBrush pill_brush(Gdiplus::Color(140, 0, 0, 0));
    canvas.FillPath(&pill_brush, &pill);

    Gdiplus::SolidBrush primary_brush(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::SolidBrush secondary_brush(Gdiplus::Color(191, 255, 255, 255));
    canvas.DrawString(primary, -1, &primary_font,
                      Gdiplus::PointF(banner.X + (width - primary_size.Width) / 2,
                                      banner.Y + static_cast<Gdiplus::REAL>(10 * scale)),
                      &primary_brush);
    canvas.DrawString(secondary, -1, &secondary_font,
                      Gdiplus::PointF(banner.X + (width - secondary_size.Width) / 2,
                                      banner.GetBottom() - secondary_size.Height -
                                          static_cast<Gdiplus::REAL>(10 * scale)),
                      &secondary_brush);
}

// A punched hole keeps a whisper of alpha where it must stay clickable:
// the picker owns every click on the overlay, but the darkened wash must
// lift off the area being indicated.
void PunchRect(Gdiplus::Graphics& canvas, const Gdiplus::RectF& rect) {
    const Gdiplus::CompositingMode previous = canvas.GetCompositingMode();
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush whisper(Gdiplus::Color(13, 0, 0, 0));
    canvas.FillRectangle(&whisper, rect);
    canvas.SetCompositingMode(previous);
}

// The overlay scene proper, described in full every time: the caller
// owns the surface, the clip that limits what actually rasterizes, and
// the push to the compositor.
void PaintPickerScene(PickerState& picker, Gdiplus::Graphics& canvas, double scale) {
    const Gdiplus::RectF bounds(0, 0, static_cast<Gdiplus::REAL>(picker.width),
                                static_cast<Gdiplus::REAL>(picker.height));
    if (picker.pin_mode) {
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
            const Gdiplus::RectF selection = SelectionRect(picker);
            Gdiplus::Pen halo(Gdiplus::Color(179, 26, 26, 26),
                              static_cast<Gdiplus::REAL>(3.0 * scale));
            canvas.DrawRectangle(&halo, selection);
            Gdiplus::Pen line(Gdiplus::Color(255, 255, 255, 255),
                              static_cast<Gdiplus::REAL>(1.0 * scale));
            canvas.DrawRectangle(&line, selection);
        }
        if (!picker.dragging) {
            // Pinning is its own tool: no mode keys here and none of the
            // region modes lead back - crossing over midway would blur
            // what a click means.
            DrawBanner(canvas, picker, L"Click or drag to pin a color",
                       L"[Shift+click] pin and continue    [Esc] done", false, scale);
        }
        return;
    }
    if (!picker.draw_mode) {
        Gdiplus::SolidBrush dim(Gdiplus::Color(51, 0, 0, 0));
        canvas.FillRectangle(&dim, bounds);
        if (picker.faces_mode) {
            // Faces are few and easy to miss: every found face is outlined
            // up front, so the answer is visible before any hovering. The
            // hovered one still gets the full accent treatment below.
            Gdiplus::Pen outline(Gdiplus::Color(217, 255, 255, 255),
                                 static_cast<Gdiplus::REAL>(1.5 * scale));
            for (int i = 0; i < static_cast<int>(picker.suggestions.size()); ++i) {
                if (i == picker.hovered) continue;
                PunchRect(canvas, picker.suggestions[static_cast<std::size_t>(i)].first);
                canvas.DrawRectangle(&outline,
                                     picker.suggestions[static_cast<std::size_t>(i)].first);
            }
        }
        if (picker.hovered >= 0 && picker.hovered < static_cast<int>(picker.suggestions.size())) {
            // Only the rectangle under the cursor is shown, washed with an
            // accent like window selection in the screenshot interfaces.
            const auto& hovered = picker.suggestions[static_cast<std::size_t>(picker.hovered)];
            PunchRect(canvas, hovered.first);
            Gdiplus::SolidBrush wash(Gdiplus::Color(64, 0, 122, 255));
            canvas.FillRectangle(&wash, hovered.first);
            Gdiplus::Pen frame(Gdiplus::Color(255, 255, 255, 255),
                               static_cast<Gdiplus::REAL>(2.0 * scale));
            canvas.DrawRectangle(&frame, hovered.first);
            const Gdiplus::FontFamily family(L"Segoe UI");
            const Gdiplus::Font font(&family, static_cast<Gdiplus::REAL>(12 * scale),
                                     Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush text(Gdiplus::Color(255, 255, 255, 255));
            canvas.DrawString(
                hovered.second.c_str(), -1, &font,
                Gdiplus::PointF(hovered.first.X + static_cast<Gdiplus::REAL>(6 * scale),
                                hovered.first.Y + static_cast<Gdiplus::REAL>(6 * scale)),
                &text);
        }
        if (picker.faces_mode) {
            DrawBanner(
                canvas, picker,
                picker.suggestions.empty() ? L"No faces found on this screen" : L"Click a face",
                L"[A] pick a window    [D] draw    [Esc] full screen", picker.suggestions.empty(),
                scale);
        } else {
            DrawBanner(canvas, picker, L"Click a window",
                       SupportsFaceDetection() ? L"[F] pick a face    [D] draw    [Esc] full screen"
                                               : L"[D] draw    [Esc] full screen",
                       false, scale);
        }
    } else {
        Gdiplus::SolidBrush dim(Gdiplus::Color(89, 0, 0, 0));
        canvas.FillRectangle(&dim, bounds);
        if (picker.dragging) {
            const Gdiplus::RectF selection = SelectionRect(picker);
            PunchRect(canvas, selection);
            Gdiplus::Pen frame(Gdiplus::Color(255, 255, 255, 255),
                               static_cast<Gdiplus::REAL>(1.5 * scale));
            canvas.DrawRectangle(&frame, selection);
        } else {
            const wchar_t* secondary = L"[Esc] full screen";
            if (!picker.windows.empty() && SupportsFaceDetection())
                secondary = L"[A] pick a window    [F] pick a face    [Esc] full screen";
            else if (!picker.windows.empty())
                secondary = L"[A] pick a window    [Esc] full screen";
            DrawBanner(canvas, picker, L"Drag to select an area", secondary, false, scale);
        }
    }
}

// Repaints the overlay. With a dirty rectangle, only that area is redrawn
// and pushed - the clip keeps the rasterizer inside the changed sliver,
// which is what makes a display-sized layered window affordable to update
// per mouse move.
void PaintPicker(PickerState& picker, const Gdiplus::RectF* dirty = nullptr) {
    if (!EnsureGdiplus()) return;
    if (!picker.surface || !picker.surface->Valid()) {
        picker.surface = std::make_unique<LayeredSurface>(picker.width, picker.height);
        if (!picker.surface->Valid()) return;
        dirty = nullptr;  // a fresh surface has no previous frame to reuse
    }
    Gdiplus::Graphics canvas(picker.surface->Dc());
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    canvas.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    const double scale = UiScale(picker.window);
    const Gdiplus::RectF bounds(0, 0, static_cast<Gdiplus::REAL>(picker.width),
                                static_cast<Gdiplus::REAL>(picker.height));
    if (dirty) canvas.SetClip(*dirty);
    // The cached surface still holds the previous frame; painting starts
    // from transparency the way it did when every paint got a fresh DIB.
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush erase(Gdiplus::Color(0, 0, 0, 0));
    canvas.FillRectangle(&erase, dirty ? *dirty : bounds);
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    PaintPickerScene(picker, canvas, scale);
    if (dirty) {
        canvas.ResetClip();
        picker.surface->PushDirty(picker.window, picker.origin_x, picker.origin_y, *dirty);
    } else {
        picker.surface->Push(picker.window, picker.origin_x, picker.origin_y);
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
void PaintPickerSelectionDelta(PickerState& picker, const Gdiplus::RectF& previous,
                               const Gdiplus::RectF& current, double scale) {
    if (!EnsureGdiplus()) return;
    if (!picker.surface || !picker.surface->Valid()) {
        PaintPicker(picker);
        return;
    }
    // Covers the frame stroke and its antialiasing on either rim.
    const auto margin = static_cast<Gdiplus::REAL>(4 * scale);
    Gdiplus::RectF outer;
    Gdiplus::RectF::Union(outer, previous, current);
    outer.Inflate(margin, margin);
    Gdiplus::RectF core;
    bool have_core = Gdiplus::RectF::Intersect(core, previous, current) != FALSE;
    if (have_core) {
        core.Inflate(-2 * margin, -2 * margin);
        have_core = core.Width > 0 && core.Height > 0;
    }
    if (!have_core) {
        PaintPicker(picker, &outer);
        return;
    }
    Gdiplus::Graphics canvas(picker.surface->Dc());
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    canvas.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::Region ring(outer);
    ring.Exclude(core);
    canvas.SetClip(&ring);
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush erase(Gdiplus::Color(0, 0, 0, 0));
    canvas.FillRectangle(&erase, outer);
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    PaintPickerScene(picker, canvas, scale);
    canvas.ResetClip();
    const Gdiplus::RectF bands[4] = {
        {outer.X, outer.Y, outer.Width, core.Y - outer.Y},
        {outer.X, core.GetBottom(), outer.Width, outer.GetBottom() - core.GetBottom()},
        {outer.X, core.Y, core.X - outer.X, core.Height},
        {core.GetRight(), core.Y, outer.GetRight() - core.GetRight(), core.Height}};
    for (const Gdiplus::RectF& band : bands) {
        if (band.Width <= 0 || band.Height <= 0) continue;
        picker.surface->PushDirty(picker.window, picker.origin_x, picker.origin_y, band);
    }
}

// 0 = pick a window, 1 = draw, 2 = pick a face, 3 = pin colors. Face
// mode is offered even when no face was found: the honest answer is the
// empty overlay saying so, not a key that silently does nothing.
void SwitchPickerMode(int mode) {
    const bool draw = mode == 1;
    const bool faces = mode == 2;
    const bool pin = mode == 3;
    // Region picking and color pinning are separate tools; a pick never
    // crosses between the families midway.
    if (!g_pickers.empty() && g_pickers.front()->pin_mode != pin) return;
    for (PickerState* picker : g_pickers) {
        if (picker->draw_mode == draw && picker->faces_mode == faces && picker->pin_mode == pin)
            continue;
        picker->draw_mode = draw;
        picker->faces_mode = faces;
        picker->pin_mode = pin;
        picker->suggestions = faces ? picker->faces : picker->windows;
        picker->hovered = -1;
        picker->dragging = false;
        picker->selection_painted = false;
        PaintPicker(*picker);
    }
}

LRESULT CALLBACK PickerProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    PickerState* state = PickerForWindow(window);
    if (!state) return DefWindowProcW(window, message, w_param, l_param);
    PickerState& picker = *state;
    switch (message) {
        case WM_SETCURSOR:
            if (picker.pin_mode) {
                SetCursor(g_pin_cursor ? g_pin_cursor : LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, picker.draw_mode ? IDC_CROSS : IDC_HAND));
            return TRUE;
        case WM_MOUSEMOVE: {
            const POINT point{static_cast<int>(static_cast<short>(LOWORD(l_param))),
                              static_cast<int>(static_cast<short>(HIWORD(l_param)))};
            if (picker.pin_mode) {
                // The swatch rides the cursor image itself; motion needs
                // no repaint here, only an active drag does.
                if ((w_param & MK_LBUTTON) == 0) return 0;
                const double scale = UiScale(window);
                const Gdiplus::RectF previous = SelectionRect(picker);
                picker.drag_current = point;
                const double threshold = 4 * scale;
                if (!picker.dragging &&
                    (std::abs(picker.drag_current.x - picker.drag_start.x) > threshold ||
                     std::abs(picker.drag_current.y - picker.drag_start.y) > threshold)) {
                    picker.dragging = true;
                    PaintPicker(picker);  // full: the banner leaves
                    return 0;
                }
                if (!picker.dragging) return 0;
                Gdiplus::RectF changed = previous;
                Gdiplus::RectF::Union(changed, changed, SelectionRect(picker));
                const auto margin = static_cast<Gdiplus::REAL>(4 * scale);
                changed.Inflate(margin, margin);
                PaintPicker(picker, &changed);
                return 0;
            }
            if (picker.draw_mode) {
                if ((w_param & MK_LBUTTON) != 0) {
                    picker.drag_current = point;
                    // A real drag only starts after a few points of travel,
                    // so a stray click never flashes a tiny selection.
                    const double scale = UiScale(window);
                    const double threshold = 4 * scale;
                    if (!picker.dragging &&
                        (std::abs(picker.drag_current.x - picker.drag_start.x) > threshold ||
                         std::abs(picker.drag_current.y - picker.drag_start.y) > threshold))
                        picker.dragging = true;
                    // Nothing on screen changes until the drag is real:
                    // the pre-threshold scene is the banner one already
                    // showing.
                    if (picker.dragging) {
                        const Gdiplus::RectF selection = SelectionRect(picker);
                        if (picker.selection_painted) {
                            PaintPickerSelectionDelta(picker, picker.painted_selection, selection,
                                                      scale);
                        } else {
                            PaintPicker(picker);  // full: the banner leaves
                        }
                        picker.painted_selection = selection;
                        picker.selection_painted = true;
                    }
                }
            } else {
                const int hovered = SuggestionAtPoint(picker, point);
                if (hovered != picker.hovered) {
                    picker.hovered = hovered;
                    PaintPicker(picker);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            picker.drag_start = {static_cast<int>(static_cast<short>(LOWORD(l_param))),
                                 static_cast<int>(static_cast<short>(HIWORD(l_param)))};
            picker.drag_current = picker.drag_start;
            SetCapture(window);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const POINT point{static_cast<int>(static_cast<short>(LOWORD(l_param))),
                              static_cast<int>(static_cast<short>(HIWORD(l_param)))};
            if (picker.pin_mode) {
                const double scale = UiScale(window);
                picker.drag_current = point;
                const Gdiplus::RectF selection = SelectionRect(picker);
                const double minimum = 8 * scale;
                if (picker.dragging && selection.Width > minimum && selection.Height > minimum) {
                    picker.pinned_area = selection;
                } else {
                    // A click pins a cursor-sized patch, not a pixel:
                    // photographs are textured, and a point sample is a
                    // lottery across the vectorscope cloud.
                    const auto span = static_cast<Gdiplus::REAL>(kPinSamplePoints * scale);
                    picker.pinned_area =
                        Gdiplus::RectF(static_cast<Gdiplus::REAL>(point.x) - span / 2,
                                       static_cast<Gdiplus::REAL>(point.y) - span / 2, span, span);
                }
                // The click's Shift carries the per-pin decision: pin
                // and keep picking, or pin and be done.
                picker.pinned_keep_open = (w_param & MK_SHIFT) != 0;
                picker.pinned_ready = true;
                if (picker.dragging) {
                    picker.dragging = false;
                    PaintPicker(picker);  // full: the frame leaves, the banner returns
                }
                return 0;
            }
            if (!picker.draw_mode) {
                const int hovered = SuggestionAtPoint(picker, point);
                if (hovered < 0) return 0;  // a miss keeps the picker open
                picker.picked = true;
                picker.confirmed = picker.suggestions[static_cast<std::size_t>(hovered)].first;
                picker.finished = true;
                return 0;
            }
            picker.drag_current = point;
            if (picker.dragging) {
                const Gdiplus::RectF selection = SelectionRect(picker);
                picker.dragging = false;
                picker.selection_painted = false;
                const double minimum = 8 * UiScale(window);
                if (selection.Width > minimum && selection.Height > minimum) {
                    picker.picked = true;
                    picker.confirmed = selection;
                    picker.finished = true;
                } else {
                    PaintPicker(picker);
                }
            }
            return 0;
        }
        case WM_KEYDOWN: {
            const int key = static_cast<int>(w_param);
            if (key == VK_ESCAPE) {
                picker.picked = false;
                picker.finished = true;
                return 0;
            }
            // Pinning is its own tool: while it is up, only ESC speaks.
            if (picker.pin_mode) return 0;
            if (key == 'A') SwitchPickerMode(0);
            if (key == 'D') SwitchPickerMode(1);
            if (key == 'F' && SupportsFaceDetection()) SwitchPickerMode(2);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
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
struct StripeTile {
    double scale = 0.0;
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
    std::unique_ptr<Gdiplus::TextureBrush> brush;
};
StripeTile g_stripe_tile;

Gdiplus::TextureBrush* StripeBrushFor(double scale) {
    if (g_stripe_tile.brush && g_stripe_tile.scale == scale) return g_stripe_tile.brush.get();
    const int period = max(1, static_cast<int>(std::lround(10.0 * scale)));
    auto bitmap = std::make_unique<Gdiplus::Bitmap>(period, period, PixelFormat32bppPARGB);
    if (bitmap->GetLastStatus() != Gdiplus::Ok) return nullptr;
    {
        Gdiplus::Graphics tile(bitmap.get());
        tile.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        tile.Clear(Gdiplus::Color(115, 26, 26, 26));
        Gdiplus::Pen stripe_pen(Gdiplus::Color(115, 230, 230, 230),
                                static_cast<Gdiplus::REAL>(4.0 * scale));
        const auto p = static_cast<Gdiplus::REAL>(period);
        const auto overhang = static_cast<Gdiplus::REAL>(4.0 * scale);
        // Strokes of constant x+y, one period apart: the same diagonal
        // the full-band loop drew.
        for (int line = -1; line <= 2; ++line) {
            const auto c = static_cast<Gdiplus::REAL>(line) * p;
            tile.DrawLine(&stripe_pen, c + overhang, -overhang, c - p - overhang, p + overhang);
        }
    }
    g_stripe_tile.scale = scale;
    g_stripe_tile.bitmap = std::move(bitmap);
    g_stripe_tile.brush = std::make_unique<Gdiplus::TextureBrush>(g_stripe_tile.bitmap.get());
    g_stripe_tile.brush->SetWrapMode(Gdiplus::WrapModeTile);
    return g_stripe_tile.brush.get();
}

// The region rectangle in border-window-local pixels.
Gdiplus::RectF BorderRegionLocal(double scale) {
    const auto pad = static_cast<Gdiplus::REAL>(kWindowPad * scale);
    return {pad, pad, static_cast<Gdiplus::REAL>(g_border.region.right - g_border.region.left),
            static_cast<Gdiplus::REAL>(g_border.region.bottom - g_border.region.top)};
}

// Eight handles, no modifier: the corners resize both axes, the edge
// midpoints resize their edge, and the rest of the band moves. The
// visible handles say which is which - a modifier key never could.
// Always visible while the border is up - hover-revealing it flickered
// on every band crossing, and crossing the band is what a cursor does
// all day. It still hides during drags and yields on tiny regions.
bool CloseVisible(double scale) {
    const Gdiplus::RectF region = BorderRegionLocal(scale);
    return g_border.drag_zone == kZoneNone && region.Width >= kMinimumWidthForClose * scale;
}

// On the band's outer corner, at forty-five degrees off the top-right
// handle dot - anchored to the corner rather than parked beside it.
Gdiplus::PointF CloseCenter(double scale) {
    const Gdiplus::RectF region = BorderRegionLocal(scale);
    return {
        static_cast<Gdiplus::REAL>(region.GetRight() + (kBorderPad - kCloseCornerInset) * scale),
        static_cast<Gdiplus::REAL>(region.Y -
                                   (kBorderPad - kCloseCornerInset + kEdgeRing) * scale)};
}

unsigned BorderZoneAtPoint(double x, double y, double scale) {
    const Gdiplus::RectF region = BorderRegionLocal(scale);
    if (region.Contains(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y)))
        return kZoneNone;  // click-through anyway
    if (CloseVisible(scale)) {
        const Gdiplus::PointF center = CloseCenter(scale);
        const double dx = x - center.X;
        const double dy = y - center.Y;
        const double hit = kCloseHitRadius * scale;
        if (dx * dx + dy * dy <= hit * hit) return kZoneClose;
    }
    const double corner = kCornerZone * scale;
    const bool near_left = x < region.X + corner;
    const bool near_right = x > region.GetRight() - corner;
    const bool near_top = y < region.Y + corner;
    const bool near_bottom = y > region.GetBottom() - corner;
    if ((near_left || near_right) && (near_top || near_bottom)) {
        unsigned zone = kZoneNone;
        zone |= near_left ? kZoneLeft : kZoneRight;
        zone |= near_top ? kZoneTop : kZoneBottom;
        return zone;
    }
    const double midpoint = kMidpointZone * scale;
    const bool mid_x = std::abs(x - (region.X + region.Width / 2)) <= midpoint;
    const bool mid_y = std::abs(y - (region.Y + region.Height / 2)) <= midpoint;
    if (mid_x && y < region.Y) return kZoneTop;
    if (mid_x && y > region.GetBottom()) return kZoneBottom;
    if (mid_y && x < region.X) return kZoneLeft;
    if (mid_y && x > region.GetRight()) return kZoneRight;
    return kZoneMove;
}

void ApplyBorderCursor(unsigned zone) {
    if (zone == kZoneNone) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return;
    }
    if ((zone & kZoneClose) != 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return;
    }
    if ((zone & kZoneMove) != 0) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return;
    }
    const bool horizontal = (zone & (kZoneLeft | kZoneRight)) != 0;
    const bool vertical = (zone & (kZoneTop | kZoneBottom)) != 0;
    if (horizontal && vertical) {
        const bool falling =
            ((zone & kZoneLeft) != 0) == ((zone & kZoneTop) != 0);  // NW-SE diagonal
        SetCursor(LoadCursorW(nullptr, falling ? IDC_SIZENWSE : IDC_SIZENESW));
        return;
    }
    SetCursor(LoadCursorW(nullptr, horizontal ? IDC_SIZEWE : IDC_SIZENS));
}

void PaintBorder() {
    if (!g_border.window || !EnsureGdiplus()) return;
    const double scale = UiScale(g_border.window);
    const auto pad = static_cast<Gdiplus::REAL>(kWindowPad * scale);
    const Gdiplus::RectF region = BorderRegionLocal(scale);
    const int width = static_cast<int>(region.Width + 2 * pad);
    const int height = static_cast<int>(region.Height + 2 * pad);
    if (!g_border.surface || g_border.surface->Width() != width ||
        g_border.surface->Height() != height) {
        g_border.surface = std::make_unique<LayeredSurface>(width, height);
    }
    LayeredSurface& surface = *g_border.surface;
    if (!surface.Valid()) return;
    g_border.painted_width = width;
    g_border.painted_height = height;
    g_border.painted_scale = scale;
    Gdiplus::Graphics canvas(surface.Dc());
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::SolidBrush erase(Gdiplus::Color(0, 0, 0, 0));
    canvas.FillRectangle(&erase, Gdiplus::RectF(0, 0, static_cast<Gdiplus::REAL>(width),
                                                static_cast<Gdiplus::REAL>(height)));
    canvas.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // The whole grab band is muted hazard tape, its own light-dark
    // alternation visible on any content. The interior is never painted
    // and therefore stays click-through.
    const auto band_pad = static_cast<Gdiplus::REAL>(kBorderPad * scale);
    const auto ring = static_cast<Gdiplus::REAL>(kEdgeRing * scale);
    Gdiplus::RectF band(region.X - band_pad, region.Y - band_pad, region.Width + 2 * band_pad,
                        region.Height + 2 * band_pad);
    // The stripes stop short of the measured-edge ring: crossing it would
    // read as the band bleeding into the measured area.
    Gdiplus::RectF stripe_hole(region.X - ring, region.Y - ring, region.Width + 2 * ring,
                               region.Height + 2 * ring);
    canvas.SetClip(band);
    canvas.ExcludeClip(stripe_hole);
    if (Gdiplus::TextureBrush* stripes = StripeBrushFor(scale)) {
        canvas.FillRectangle(stripes, band);
    } else {
        // Out of memory for a tile the size of a coin; the plain base
        // keeps the band visible.
        Gdiplus::SolidBrush band_brush(Gdiplus::Color(115, 26, 26, 26));
        canvas.FillRectangle(&band_brush, band);
    }
    canvas.ResetClip();

    // The measured edge is a filled ring spanning exactly from the region
    // to the stripes, with white dashes riding over the dark base so one
    // of the two tones survives any background.
    canvas.SetClip(stripe_hole);
    canvas.ExcludeClip(region);
    Gdiplus::SolidBrush ring_brush(Gdiplus::Color(217, 26, 26, 26));
    canvas.FillRectangle(&ring_brush, stripe_hole);
    canvas.ResetClip();
    Gdiplus::Pen dash_pen(Gdiplus::Color(242, 247, 247, 247), ring);
    const Gdiplus::REAL dash_pattern[2] = {
        static_cast<Gdiplus::REAL>(4.0 * scale / (ring > 0 ? ring : 1)),
        static_cast<Gdiplus::REAL>(4.0 * scale / (ring > 0 ? ring : 1))};
    dash_pen.SetDashPattern(dash_pattern, 2);
    canvas.DrawRectangle(&dash_pen, Gdiplus::RectF(region.X - ring / 2, region.Y - ring / 2,
                                                   region.Width + ring, region.Height + ring));

    // Eight handle dots - corners and edge midpoints - centered on the
    // measurement line: small gray circles straddling the selection edge.
    const auto radius = static_cast<Gdiplus::REAL>(kHandleRadius * scale);
    const auto handle = [&](Gdiplus::REAL x, Gdiplus::REAL y) {
        const Gdiplus::RectF circle(x - radius, y - radius, radius * 2, radius * 2);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 199, 199, 199));
        canvas.FillEllipse(&fill, circle);
        // A dark rim beneath the near-white ring keeps the dot visible on
        // a bright sky; the ring matches the measurement line, so the dots
        // and the line read as one instrument.
        Gdiplus::Pen rim(Gdiplus::Color(179, 26, 26, 26), static_cast<Gdiplus::REAL>(2.0 * scale));
        canvas.DrawEllipse(&rim, circle);
        Gdiplus::Pen bright(Gdiplus::Color(242, 247, 247, 247),
                            static_cast<Gdiplus::REAL>(1.0 * scale));
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

    // The hover-revealed close button, in the handles' own visual
    // language: a dark disc where the dots are light, so it reads as an
    // action rather than a grip, with the same bright ring and an x.
    if (CloseVisible(scale)) {
        const Gdiplus::PointF center = CloseCenter(scale);
        const auto close_radius = static_cast<Gdiplus::REAL>(kCloseRadius * scale);
        const Gdiplus::RectF disc(center.X - close_radius, center.Y - close_radius,
                                  close_radius * 2, close_radius * 2);
        Gdiplus::SolidBrush disc_brush(Gdiplus::Color(217, 26, 26, 26));
        canvas.FillEllipse(&disc_brush, disc);
        Gdiplus::Pen disc_ring(Gdiplus::Color(242, 247, 247, 247),
                               static_cast<Gdiplus::REAL>(1.0 * scale));
        canvas.DrawEllipse(&disc_ring, disc);
        const auto arm = static_cast<Gdiplus::REAL>((kCloseRadius - 3.7) * scale);
        Gdiplus::Pen cross(Gdiplus::Color(242, 247, 247, 247),
                           static_cast<Gdiplus::REAL>(1.3 * scale));
        cross.SetStartCap(Gdiplus::LineCapRound);
        cross.SetEndCap(Gdiplus::LineCapRound);
        canvas.DrawLine(&cross, center.X - arm, center.Y - arm, center.X + arm, center.Y + arm);
        canvas.DrawLine(&cross, center.X - arm, center.Y + arm, center.X + arm, center.Y - arm);
    }

    surface.Push(g_border.window, g_border.region.left - static_cast<int>(kWindowPad * scale),
                 g_border.region.top - static_cast<int>(kWindowPad * scale));
}

LRESULT CALLBACK BorderProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_MOUSEACTIVATE:
            // The band is mouse-only. WS_EX_NOACTIVATE keeps it from
            // BECOMING the foreground window, but without this answer a
            // click still DEACTIVATES whichever window holds the
            // keyboard - every shortcut dead until the scope window is
            // clicked again, with the band taking nothing in exchange.
            return MA_NOACTIVATE;
        case WM_NCHITTEST: {
            // The interior is the editor's, not ours; only the band takes
            // the mouse.
            const double scale = UiScale(window);
            RECT frame{};
            GetWindowRect(window, &frame);
            const double x = static_cast<short>(LOWORD(l_param)) - frame.left;
            const double y = static_cast<short>(HIWORD(l_param)) - frame.top;
            return BorderZoneAtPoint(x, y, scale) == kZoneNone ? HTTRANSPARENT : HTCLIENT;
        }
        case WM_SETCURSOR: {
            if (g_border.drag_zone != kZoneNone) {
                ApplyBorderCursor(g_border.drag_zone);
                return TRUE;
            }
            POINT cursor{};
            GetCursorPos(&cursor);
            RECT frame{};
            GetWindowRect(window, &frame);
            const double scale = UiScale(window);
            ApplyBorderCursor(
                BorderZoneAtPoint(cursor.x - frame.left, cursor.y - frame.top, scale));
            return TRUE;
        }
        case WM_LBUTTONDBLCLK: {
            // Double-clicking anywhere on the band dismisses the region -
            // the fast path once the close button has taught the
            // gesture's home.
            const double scale = UiScale(window);
            const double x = static_cast<short>(LOWORD(l_param));
            const double y = static_cast<short>(HIWORD(l_param));
            if (BorderZoneAtPoint(x, y, scale) != kZoneNone) g_border_dismissed = true;
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const double scale = UiScale(window);
            const double x = static_cast<short>(LOWORD(l_param));
            const double y = static_cast<short>(HIWORD(l_param));
            const unsigned zone = BorderZoneAtPoint(x, y, scale);
            if (zone == kZoneNone) return 0;
            if ((zone & kZoneClose) != 0) {
                g_border.close_pressed = true;
                return 0;
            }
            g_border.drag_zone = zone;
            GetCursorPos(&g_border.drag_start_mouse);
            g_border.drag_start_region = g_border.region;
            g_border_editing = true;
            SetCapture(window);
            PaintBorder();  // the close button hides while dragging
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_border.drag_zone == kZoneNone) return 0;
            // Screen coordinates throughout: the window itself moves as
            // the application applies each edit, so client coordinates
            // shift under the cursor mid-drag.
            POINT mouse{};
            GetCursorPos(&mouse);
            const int dx = mouse.x - g_border.drag_start_mouse.x;
            const int dy = mouse.y - g_border.drag_start_mouse.y;
            RECT rect = g_border.drag_start_region;
            const double scale = UiScale(window);
            const auto minimum = static_cast<int>(kMinimumRegionSize * scale);
            if ((g_border.drag_zone & kZoneMove) != 0) {
                OffsetRect(&rect, dx, dy);
            } else {
                if ((g_border.drag_zone & kZoneLeft) != 0)
                    rect.left = std::min(g_border.drag_start_region.left + dx,
                                         g_border.drag_start_region.right - minimum);
                if ((g_border.drag_zone & kZoneRight) != 0)
                    rect.right = std::max(g_border.drag_start_region.right + dx,
                                          g_border.drag_start_region.left + minimum);
                if ((g_border.drag_zone & kZoneTop) != 0)
                    rect.top = std::min(g_border.drag_start_region.top + dy,
                                        g_border.drag_start_region.bottom - minimum);
                if ((g_border.drag_zone & kZoneBottom) != 0)
                    rect.bottom = std::max(g_border.drag_start_region.bottom + dy,
                                           g_border.drag_start_region.top + minimum);
            }
            RegionOfInterest region;
            region.left_percent =
                (rect.left - g_border.display_origin_x) / g_border.display_width * 100.0;
            region.top_percent =
                (rect.top - g_border.display_origin_y) / g_border.display_height * 100.0;
            region.right_percent =
                (rect.right - g_border.display_origin_x) / g_border.display_width * 100.0;
            region.bottom_percent =
                (rect.bottom - g_border.display_origin_y) / g_border.display_height * 100.0;
            g_border_edit_region = region;
            g_border_edit_changed = true;
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_border.close_pressed) {
                g_border.close_pressed = false;
                const double scale = UiScale(window);
                const double x = static_cast<short>(LOWORD(l_param));
                const double y = static_cast<short>(HIWORD(l_param));
                if ((BorderZoneAtPoint(x, y, scale) & kZoneClose) != 0) g_border_dismissed = true;
                return 0;
            }
            if (g_border.drag_zone != kZoneNone) {
                g_border.drag_zone = kZoneNone;
                g_border_editing = false;
                ReleaseCapture();
                PaintBorder();
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

HWND CreateOverlayWindow(const wchar_t* class_name, WNDPROC procedure, DWORD ex_style,
                         UINT class_style) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = class_style;
    window_class.lpfnWndProc = procedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    window_class.hCursor = nullptr;   // WM_SETCURSOR decides
    RegisterClassExW(&window_class);  // idempotent; re-registration fails harmlessly
    HWND window = CreateWindowExW(ex_style, class_name, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  instance, nullptr);
    // Our own surfaces must never reach the scopes; duplication has no
    // application-level exclusion, so each window excludes itself. Best
    // effort: unsupported before Windows 10 2004.
    if (window) SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
    return window;
}

}  // namespace

bool BeginRegionPick(const std::vector<PickerDisplay>& displays, RegionPickerMode initial_mode) {
    if (!g_pickers.empty()) return false;  // one picker at a time

    // A window pick with nothing to pick anywhere opens as drawing, like
    // before, but the decision is global so every display shows the same
    // mode.
    bool any_windows = false;
    for (const PickerDisplay& entry : displays) any_windows |= !entry.windows.empty();
    const bool pin = initial_mode == RegionPickerMode::PinColor;
    const bool draw = !pin && (initial_mode == RegionPickerMode::Draw ||
                               (initial_mode == RegionPickerMode::PickWindows && !any_windows));
    const bool faces = initial_mode == RegionPickerMode::PickFaces;

    for (const PickerDisplay& entry : displays) {
        const auto geometry = GeometryOfDisplay(entry.display_id);
        if (!geometry) continue;  // gone between enumeration and now

        auto* picker = new PickerState;
        picker->display_id = entry.display_id;
        picker->origin_x = static_cast<int>(geometry->origin_x);
        picker->origin_y = static_cast<int>(geometry->origin_y);
        picker->width = static_cast<int>(geometry->width_points);
        picker->height = static_cast<int>(geometry->height_points);
        for (const SuggestedRegion& suggestion : entry.windows)
            picker->windows.emplace_back(
                LocalRectFromRegion(suggestion.region, picker->width, picker->height),
                WideFromUtf8(suggestion.label));
        for (const SuggestedRegion& suggestion : entry.faces)
            picker->faces.emplace_back(
                LocalRectFromRegion(suggestion.region, picker->width, picker->height),
                WideFromUtf8(suggestion.label));
        picker->draw_mode = draw;
        picker->faces_mode = faces;
        picker->pin_mode = pin;
        if (!draw && !pin) picker->suggestions = faces ? picker->faces : picker->windows;

        picker->window = CreateOverlayWindow(L"SidescopesPickerOverlay", PickerProc,
                                             WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, 0);
        if (!picker->window) {
            delete picker;
            continue;
        }
        g_pickers.push_back(picker);
        SetWindowPos(picker->window, HWND_TOPMOST, picker->origin_x, picker->origin_y,
                     picker->width, picker->height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    if (g_pickers.empty()) return false;

    // This application's own visible windows float above the overlays for
    // the duration: they stay undimmed and clickable by ordinary window
    // compositing. Their rectangles still feed the banner placement,
    // which avoids sitting beneath them. They are topmost already (the
    // scope window floats), so there is no level to restore afterwards.
    for (HWND window : OwnWindows())
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // The keyboard starts on the display under the cursor - that is where
    // the user's attention is - and follows clicks after.
    PickerState* key_picker = g_pickers.front();
    const uint32_t cursor_display = DisplayUnderCursor().value_or(0);
    for (PickerState* picker : g_pickers) {
        picker->exclusions = OwnWindowExclusions(*picker);
        if (picker->display_id == cursor_display) key_picker = picker;
    }
    SetForegroundWindow(key_picker->window);
    SetFocus(key_picker->window);
    for (PickerState* picker : g_pickers) PaintPicker(*picker);
    return true;
}

RegionPickPoll PollRegionPick() {
    RegionPickPoll poll;
    if (g_pickers.empty()) return poll;
    poll.active = true;
    // Mode flags come first: the finishing poll returns early below, and
    // the caller needs them to know a pin-mode finish never means a
    // region change. The pickers switch modes in lockstep; the front one
    // speaks for all.
    poll.pin_mode = g_pickers.front()->pin_mode;
    for (PickerState* picker : g_pickers) {
        if (!picker->pinned_ready) continue;
        picker->pinned_ready = false;
        poll.pinned_area = RegionFromLocalRect(picker->pinned_area, picker->width, picker->height);
        poll.pinned_keep_open = picker->pinned_keep_open;
        poll.display_id = picker->display_id;
        break;
    }

    // The banner dodges this application's own windows; their rectangles
    // refresh on a gentle cadence - nothing visual tracks them, so
    // per-frame repaints would be waste.
    static ULONGLONG last_exclusion_refresh = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last_exclusion_refresh > 200) {
        last_exclusion_refresh = now;
        for (PickerState* picker : g_pickers) {
            std::vector<Gdiplus::RectF> exclusions = OwnWindowExclusions(*picker);
            if (exclusions.size() != picker->exclusions.size() ||
                !std::equal(
                    exclusions.begin(), exclusions.end(), picker->exclusions.begin(),
                    [](const Gdiplus::RectF& a, const Gdiplus::RectF& b) { return a.Equals(b); })) {
                picker->exclusions = std::move(exclusions);
                PaintPicker(*picker);
            }
        }
    }

    // Any overlay finishing - a confirm there, or ESC anywhere - ends the
    // pick on every display.
    for (PickerState* finished_picker : g_pickers) {
        if (!finished_picker->finished) continue;
        poll.finished = true;
        poll.display_id = finished_picker->display_id;
        if (finished_picker->picked)
            poll.confirmed = RegionFromLocalRect(finished_picker->confirmed, finished_picker->width,
                                                 finished_picker->height);
        for (PickerState* picker : g_pickers) {
            DestroyWindow(picker->window);
            delete picker;
        }
        g_pickers.clear();
        if (g_pin_cursor) {
            DestroyIcon(g_pin_cursor);
            g_pin_cursor = nullptr;
        }
        return poll;
    }

    // The cursor is only ever on one display, so at most one overlay has
    // something to preview.
    for (PickerState* picker : g_pickers) {
        if (picker->pin_mode) break;  // pin modes never preview a region
        if (!picker->draw_mode) {
            if (picker->hovered >= 0 &&
                picker->hovered < static_cast<int>(picker->suggestions.size())) {
                poll.preview = RegionFromLocalRect(
                    picker->suggestions[static_cast<std::size_t>(picker->hovered)].first,
                    picker->width, picker->height);
                poll.display_id = picker->display_id;
                break;
            }
        } else if (picker->dragging) {
            const Gdiplus::RectF selection = SelectionRect(*picker);
            const double minimum = 8 * UiScale(picker->window);
            if (selection.Width > minimum && selection.Height > minimum) {
                poll.preview = RegionFromLocalRect(selection, picker->width, picker->height);
                poll.display_id = picker->display_id;
                break;
            }
        }
    }
    return poll;
}

void CancelRegionPick() {
    if (g_pickers.empty()) return;
    g_pickers.front()->picked = false;
    g_pickers.front()->finished = true;
}

void SetRegionPickMode(RegionPickerMode mode) {
    SwitchPickerMode(mode == RegionPickerMode::Draw        ? 1
                     : mode == RegionPickerMode::PickFaces ? 2
                     : mode == RegionPickerMode::PinColor  ? 3
                                                           : 0);
}

void SetRegionPickChipColor(const std::optional<FloatColor>& color) {
    const bool color_changed = color.has_value() != g_pin_chip_color.has_value() ||
                               (color && g_pin_chip_color &&
                                (std::lround(color->r) != std::lround(g_pin_chip_color->r) ||
                                 std::lround(color->g) != std::lround(g_pin_chip_color->g) ||
                                 std::lround(color->b) != std::lround(g_pin_chip_color->b)));
    g_pin_chip_color = color;
    if (g_pickers.empty() || !g_pickers.front()->pin_mode) return;
    if (g_pin_cursor && !color_changed) return;
    if (!EnsureGdiplus()) return;
    HCURSOR next = BuildPinCursor(UiScale(g_pickers.front()->window), color);
    if (!next) return;
    HCURSOR previous = g_pin_cursor;
    g_pin_cursor = next;
    // Apply immediately while one of the overlays owns the mouse; the
    // next WM_SETCURSOR keeps it applied afterwards.
    POINT cursor_position{};
    GetCursorPos(&cursor_position);
    if (PickerForWindow(WindowFromPoint(cursor_position)) || PickerForWindow(GetCapture()))
        SetCursor(g_pin_cursor);
    if (previous) DestroyIcon(previous);
}

void ShowRegionBorder(uint32_t display_id, const RegionOfInterest& region) {
    const auto geometry = GeometryOfDisplay(display_id);
    if (!geometry) return;

    if (!g_border.window) {
        // CS_DBLCLKS so the band can take the double-click dismissal.
        g_border.window = CreateOverlayWindow(
            L"SidescopesRegionBorder", BorderProc,
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CS_DBLCLKS);
        if (!g_border.window) return;
    }

    g_border.display_origin_x = geometry->origin_x;
    g_border.display_origin_y = geometry->origin_y;
    g_border.display_width = geometry->width_points;
    g_border.display_height = geometry->height_points;
    g_border.region.left =
        static_cast<int>(geometry->origin_x + region.left_percent / 100.0 * geometry->width_points);
    g_border.region.top =
        static_cast<int>(geometry->origin_y + region.top_percent / 100.0 * geometry->height_points);
    g_border.region.right = static_cast<int>(geometry->origin_x +
                                             region.right_percent / 100.0 * geometry->width_points);
    g_border.region.bottom = static_cast<int>(geometry->origin_y + region.bottom_percent / 100.0 *
                                                                       geometry->height_points);

    const double scale = UiScale(g_border.window);
    const auto pad = static_cast<int>(kWindowPad * scale);
    // Directly beneath the scope window when one is visible: the border
    // must never cover the scopes, and both live in the topmost band.
    HWND insert_after = HWND_TOPMOST;
    const std::vector<HWND> own = OwnWindows();
    if (!own.empty()) insert_after = own.front();
    const int width = (g_border.region.right - g_border.region.left) + 2 * pad;
    const int height = (g_border.region.bottom - g_border.region.top) + 2 * pad;
    SetWindowPos(g_border.window, insert_after, g_border.region.left - pad,
                 g_border.region.top - pad, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // The surface only shows size and scale; while the region is dragged
    // around whole - or this sync fires for an unrelated settings change -
    // moving the window above is the entire job.
    if (width != g_border.painted_width || height != g_border.painted_height ||
        scale != g_border.painted_scale)
        PaintBorder();
}

void HideRegionBorder() {
    if (g_border.window) ShowWindow(g_border.window, SW_HIDE);
}

RegionBorderEdit PollRegionBorderEdit() {
    RegionBorderEdit edit;
    edit.editing = g_border_editing;
    edit.dismissed = g_border_dismissed;
    g_border_dismissed = false;
    if (g_border_edit_changed) {
        edit.region = g_border_edit_region;
        g_border_edit_changed = false;
    }
    return edit;
}

}  // namespace sidescopes
