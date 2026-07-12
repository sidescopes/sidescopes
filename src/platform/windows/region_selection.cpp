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

// Which edges a border drag adjusts; Move relocates the whole region.
enum ZoneBits : unsigned {
    kZoneNone = 0,
    kZoneLeft = 1u << 0,
    kZoneRight = 1u << 1,
    kZoneTop = 1u << 2,
    kZoneBottom = 1u << 3,
    kZoneMove = 1u << 4,
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
// a GDI+ surface, pushed to the window with per-pixel alpha.
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

    void Push(HWND window, int screen_x, int screen_y) {
        POINT position{screen_x, screen_y};
        SIZE size{width_, height_};
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(window, nullptr, &position, &size, dc_, &source, 0, &blend, ULW_ALPHA);
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
    int origin_x = 0;  // the covered monitor, virtual-screen pixels
    int origin_y = 0;
    int width = 0;
    int height = 0;
    bool draw_mode = false;
    bool faces_mode = false;
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
};

PickerState* g_picker = nullptr;

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
};

BorderState g_border;

// Shared edit state the application polls once per frame.
bool g_border_editing = false;
bool g_border_edit_changed = false;
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
            if (g_picker && window == g_picker->window) return TRUE;
            if (window == g_border.window) return TRUE;
            state->windows->push_back(window);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&collector));
    return windows;
}

std::vector<Gdiplus::RectF> OwnWindowExclusions() {
    std::vector<Gdiplus::RectF> exclusions;
    if (!g_picker) return exclusions;
    for (HWND window : OwnWindows()) {
        RECT rect{};
        if (!GetWindowRect(window, &rect)) continue;
        exclusions.emplace_back(static_cast<Gdiplus::REAL>(rect.left - g_picker->origin_x),
                                static_cast<Gdiplus::REAL>(rect.top - g_picker->origin_y),
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

void PaintPicker() {
    if (!g_picker || !EnsureGdiplus()) return;
    PickerState& picker = *g_picker;
    LayeredSurface surface(picker.width, picker.height);
    if (!surface.Valid()) return;
    Gdiplus::Graphics canvas(surface.Dc());
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    canvas.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    const double scale = UiScale(picker.window);

    const Gdiplus::RectF bounds(0, 0, static_cast<Gdiplus::REAL>(picker.width),
                                static_cast<Gdiplus::REAL>(picker.height));
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

    surface.Push(picker.window, picker.origin_x, picker.origin_y);
}

// 0 = pick a window, 1 = draw, 2 = pick a face. Face mode is offered even
// when no face was found: the honest answer is the empty overlay saying
// so, not a key that silently does nothing.
void SwitchPickerMode(int mode) {
    if (!g_picker) return;
    const bool draw = mode == 1;
    const bool faces = mode == 2;
    if (g_picker->draw_mode == draw && g_picker->faces_mode == faces) return;
    g_picker->draw_mode = draw;
    g_picker->faces_mode = faces;
    g_picker->suggestions = faces ? g_picker->faces : g_picker->windows;
    g_picker->hovered = -1;
    g_picker->dragging = false;
    PaintPicker();
}

LRESULT CALLBACK PickerProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (!g_picker || window != g_picker->window)
        return DefWindowProcW(window, message, w_param, l_param);
    PickerState& picker = *g_picker;
    switch (message) {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, picker.draw_mode ? IDC_CROSS : IDC_HAND));
            return TRUE;
        case WM_MOUSEMOVE: {
            const POINT point{static_cast<int>(static_cast<short>(LOWORD(l_param))),
                              static_cast<int>(static_cast<short>(HIWORD(l_param)))};
            if (picker.draw_mode) {
                if ((w_param & MK_LBUTTON) != 0) {
                    picker.drag_current = point;
                    // A real drag only starts after a few points of travel,
                    // so a stray click never flashes a tiny selection.
                    const double threshold = 4 * UiScale(window);
                    if (!picker.dragging &&
                        (std::abs(picker.drag_current.x - picker.drag_start.x) > threshold ||
                         std::abs(picker.drag_current.y - picker.drag_start.y) > threshold))
                        picker.dragging = true;
                    PaintPicker();
                }
            } else {
                const int hovered = SuggestionAtPoint(picker, point);
                if (hovered != picker.hovered) {
                    picker.hovered = hovered;
                    PaintPicker();
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
                const double minimum = 8 * UiScale(window);
                if (selection.Width > minimum && selection.Height > minimum) {
                    picker.picked = true;
                    picker.confirmed = selection;
                    picker.finished = true;
                } else {
                    PaintPicker();
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

// The region rectangle in border-window-local pixels.
Gdiplus::RectF BorderRegionLocal(double scale) {
    const auto pad = static_cast<Gdiplus::REAL>(kWindowPad * scale);
    return {pad, pad, static_cast<Gdiplus::REAL>(g_border.region.right - g_border.region.left),
            static_cast<Gdiplus::REAL>(g_border.region.bottom - g_border.region.top)};
}

// Eight handles, no modifier: the corners resize both axes, the edge
// midpoints resize their edge, and the rest of the band moves. The
// visible handles say which is which - a modifier key never could.
unsigned BorderZoneAtPoint(double x, double y, double scale) {
    const Gdiplus::RectF region = BorderRegionLocal(scale);
    if (region.Contains(static_cast<Gdiplus::REAL>(x), static_cast<Gdiplus::REAL>(y)))
        return kZoneNone;  // click-through anyway
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
    LayeredSurface surface(width, height);
    if (!surface.Valid()) return;
    Gdiplus::Graphics canvas(surface.Dc());
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
    Gdiplus::SolidBrush band_brush(Gdiplus::Color(115, 26, 26, 26));
    canvas.FillRectangle(&band_brush, band);
    Gdiplus::Pen stripe_pen(Gdiplus::Color(115, 230, 230, 230),
                            static_cast<Gdiplus::REAL>(4.0 * scale));
    const auto diagonal = static_cast<Gdiplus::REAL>(height);
    for (Gdiplus::REAL x = -diagonal; x < static_cast<Gdiplus::REAL>(width);
         x += static_cast<Gdiplus::REAL>(10.0 * scale))
        canvas.DrawLine(&stripe_pen, x, static_cast<Gdiplus::REAL>(height), x + diagonal, 0.0f);
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

    surface.Push(g_border.window, g_border.region.left - static_cast<int>(kWindowPad * scale),
                 g_border.region.top - static_cast<int>(kWindowPad * scale));
}

LRESULT CALLBACK BorderProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
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
        case WM_LBUTTONDOWN: {
            const double scale = UiScale(window);
            const double x = static_cast<short>(LOWORD(l_param));
            const double y = static_cast<short>(HIWORD(l_param));
            g_border.drag_zone = BorderZoneAtPoint(x, y, scale);
            if (g_border.drag_zone == kZoneNone) return 0;
            GetCursorPos(&g_border.drag_start_mouse);
            g_border.drag_start_region = g_border.region;
            g_border_editing = true;
            SetCapture(window);
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
        case WM_LBUTTONUP:
            if (g_border.drag_zone != kZoneNone) {
                g_border.drag_zone = kZoneNone;
                g_border_editing = false;
                ReleaseCapture();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

HWND CreateOverlayWindow(const wchar_t* class_name, WNDPROC procedure, DWORD ex_style) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
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

bool BeginRegionPick(uint32_t display_id, const std::vector<SuggestedRegion>& windows,
                     const std::vector<SuggestedRegion>& faces, RegionPickerMode initial_mode) {
    if (g_picker) return false;  // one picker at a time
    const auto geometry = GeometryOfDisplay(display_id);
    if (!geometry) return false;

    auto* picker = new PickerState;
    picker->origin_x = static_cast<int>(geometry->origin_x);
    picker->origin_y = static_cast<int>(geometry->origin_y);
    picker->width = static_cast<int>(geometry->width_points);
    picker->height = static_cast<int>(geometry->height_points);
    for (const SuggestedRegion& suggestion : windows)
        picker->windows.emplace_back(
            LocalRectFromRegion(suggestion.region, picker->width, picker->height),
            WideFromUtf8(suggestion.label));
    for (const SuggestedRegion& suggestion : faces)
        picker->faces.emplace_back(
            LocalRectFromRegion(suggestion.region, picker->width, picker->height),
            WideFromUtf8(suggestion.label));
    if (initial_mode == RegionPickerMode::Draw ||
        (initial_mode == RegionPickerMode::PickWindows && windows.empty())) {
        picker->draw_mode = true;
    } else if (initial_mode == RegionPickerMode::PickFaces) {
        picker->faces_mode = true;
        picker->suggestions = picker->faces;
    } else {
        picker->suggestions = picker->windows;
    }

    picker->window = CreateOverlayWindow(L"SidescopesPickerOverlay", PickerProc,
                                         WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    if (!picker->window) {
        delete picker;
        return false;
    }
    g_picker = picker;
    SetWindowPos(picker->window, HWND_TOPMOST, picker->origin_x, picker->origin_y, picker->width,
                 picker->height, SWP_SHOWWINDOW);
    // This application's own visible windows float above the overlay for
    // the duration: they stay undimmed and clickable by ordinary window
    // compositing. Their rectangles still feed the banner placement,
    // which avoids sitting beneath them. They are topmost already (the
    // scope window floats), so there is no level to restore afterwards.
    for (HWND window : OwnWindows())
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    picker->exclusions = OwnWindowExclusions();
    // The overlay owns the keyboard for ESC and the mode keys.
    SetForegroundWindow(picker->window);
    SetFocus(picker->window);
    PaintPicker();
    return true;
}

RegionPickPoll PollRegionPick() {
    RegionPickPoll poll;
    if (!g_picker) return poll;
    poll.active = true;

    // The banner dodges this application's own windows; their rectangles
    // refresh on a gentle cadence - nothing visual tracks them, so
    // per-frame repaints would be waste.
    static ULONGLONG last_exclusion_refresh = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last_exclusion_refresh > 200) {
        last_exclusion_refresh = now;
        std::vector<Gdiplus::RectF> exclusions = OwnWindowExclusions();
        if (exclusions.size() != g_picker->exclusions.size() ||
            !std::equal(
                exclusions.begin(), exclusions.end(), g_picker->exclusions.begin(),
                [](const Gdiplus::RectF& a, const Gdiplus::RectF& b) { return a.Equals(b); })) {
            g_picker->exclusions = std::move(exclusions);
            PaintPicker();
        }
    }

    if (g_picker->finished) {
        poll.finished = true;
        if (g_picker->picked)
            poll.confirmed =
                RegionFromLocalRect(g_picker->confirmed, g_picker->width, g_picker->height);
        DestroyWindow(g_picker->window);
        delete g_picker;
        g_picker = nullptr;
        return poll;
    }

    if (!g_picker->draw_mode) {
        if (g_picker->hovered >= 0 &&
            g_picker->hovered < static_cast<int>(g_picker->suggestions.size()))
            poll.preview = RegionFromLocalRect(
                g_picker->suggestions[static_cast<std::size_t>(g_picker->hovered)].first,
                g_picker->width, g_picker->height);
    } else if (g_picker->dragging) {
        const Gdiplus::RectF selection = SelectionRect(*g_picker);
        const double minimum = 8 * UiScale(g_picker->window);
        if (selection.Width > minimum && selection.Height > minimum)
            poll.preview = RegionFromLocalRect(selection, g_picker->width, g_picker->height);
    }
    return poll;
}

void CancelRegionPick() {
    if (!g_picker) return;
    g_picker->picked = false;
    g_picker->finished = true;
}

void SetRegionPickMode(RegionPickerMode mode) {
    if (!g_picker) return;
    SwitchPickerMode(mode == RegionPickerMode::Draw        ? 1
                     : mode == RegionPickerMode::PickFaces ? 2
                                                           : 0);
}

void ShowRegionBorder(uint32_t display_id, const RegionOfInterest& region) {
    const auto geometry = GeometryOfDisplay(display_id);
    if (!geometry) return;

    if (!g_border.window) {
        g_border.window = CreateOverlayWindow(
            L"SidescopesRegionBorder", BorderProc,
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
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
    SetWindowPos(
        g_border.window, insert_after, g_border.region.left - pad, g_border.region.top - pad,
        (g_border.region.right - g_border.region.left) + 2 * pad,
        (g_border.region.bottom - g_border.region.top) + 2 * pad, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    PaintBorder();
}

void HideRegionBorder() {
    if (g_border.window) ShowWindow(g_border.window, SW_HIDE);
}

RegionBorderEdit PollRegionBorderEdit() {
    RegionBorderEdit edit;
    edit.editing = g_border_editing;
    if (g_border_edit_changed) {
        edit.region = g_border_edit_region;
        g_border_edit_changed = false;
    }
    return edit;
}

}  // namespace sidescopes
