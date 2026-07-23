#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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

#include "platform/region_geometry.h"

namespace sidescopes {

inline bool ensureGdiplus()
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
inline double uiScale(HWND window)
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
inline LocalRect toLocalRect(const Gdiplus::RectF& rect)
{
    return {rect.X, rect.Y, rect.Width, rect.Height};
}

inline LocalRect toLocalRect(const RECT& rect)
{
    return {static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right - rect.left),
            static_cast<double>(rect.bottom - rect.top)};
}

inline Gdiplus::RectF toRectF(const LocalRect& rect)
{
    return {static_cast<Gdiplus::REAL>(rect.x), static_cast<Gdiplus::REAL>(rect.y),
            static_cast<Gdiplus::REAL>(rect.width), static_cast<Gdiplus::REAL>(rect.height)};
}

}  // namespace sidescopes
