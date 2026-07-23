#include "platform/windows/region_border_view.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/diagnostics.h"
#include "platform/icons.h"
#include "platform/region_geometry.h"
#include "platform/windows/region_overlay_surface.h"

namespace sidescopes {

// ---------------------------------------------------------------------------
// Region border
// ---------------------------------------------------------------------------

BorderState g_border;

// Shared edit state the application polls once per frame.
bool g_borderEditing = false;
bool g_borderEditChanged = false;
bool g_borderDismissed = false;
bool g_borderAttachToggled = false;
RegionOfInterest g_borderEditRegion;

namespace {

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
    return {static_cast<Gdiplus::REAL>(region.X + (TabAttachZone / 2 - BorderPad) * scale),
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
    // read as the band bleeding into the measured region.
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
// The attached window's name rides the band above the top edge: the attached
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
    const Gdiplus::FontFamily family(L"Segoe UI");
    const Gdiplus::Font font(&family, static_cast<Gdiplus::REAL>(10.0 * scale), Gdiplus::FontStyleRegular,
                             Gdiplus::UnitPixel);
    Gdiplus::RectF measured;
    canvas.MeasureString(g_border.attachedLabel.c_str(), -1, &font, Gdiplus::PointF(0, 0), &measured);
    const auto padX = static_cast<Gdiplus::REAL>(6.0 * scale);
    const auto padY = static_cast<Gdiplus::REAL>(2.0 * scale);
    const auto triangleZone = static_cast<Gdiplus::REAL>(TabAttachZone * scale);
    // The tab holds the attach toggle at its fixed left end, then the
    // text; left-aligned with the region's own corner. The shared layout
    // keeps both platforms' degradation on small regions identical.
    // Flush with the band's outer left edge: the tab and the border read
    // as one left-aligned block.
    const Gdiplus::REAL tabX = region.X - static_cast<Gdiplus::REAL>(BorderPad * scale);
    // The trim budget ends at the band's outer right edge, so a truncated
    // tab stays flush with the border block on both sides.
    const TabLayout layout =
        borderTabLayout(region.X + region.Width + static_cast<Gdiplus::REAL>(BorderPad * scale) - tabX, triangleZone,
                        padX, measured.Width, 16.0 * scale);
    if (!layout.visible) {
        return;
    }
    const auto textWidth = static_cast<Gdiplus::REAL>(layout.textWidth);
    const auto tabWidth = static_cast<Gdiplus::REAL>(layout.tabWidth);
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

}  // namespace

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

namespace {

int borderSettleInset(const RECT& target, double scale)
{
    return static_cast<int>(
        std::min({BorderSettlePoints * scale, (target.right - target.left) / 6.0, (target.bottom - target.top) / 6.0}));
}

}  // namespace

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

namespace {

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
    SS_DIAG(Border, "advance alpha=%d target=%ld,%ld,%ld,%ld", static_cast<int>(g_border.alpha),
            g_border.appearTarget.left, g_border.appearTarget.top, g_border.appearTarget.right,
            g_border.appearTarget.bottom);
    // Paint first, as in presentBorderWindow: the push places the surface.
    paintBorder();
    SetWindowPos(g_border.window, nullptr, g_border.region.left - pad, g_border.region.top - pad - strip, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
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

}  // namespace

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

}  // namespace sidescopes
