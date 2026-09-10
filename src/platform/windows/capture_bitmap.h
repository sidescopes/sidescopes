#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace sidescopes {

// A screenshot's native storage outlives the copy into owned frame pixels.
// Release it on every exit, including an allocation failure during that copy.
class CaptureBitmap
{
public:
    CaptureBitmap(int width, int height)
    {
        if (width <= 0 || height <= 0) {
            return;
        }
        m_screen = GetDC(nullptr);
        m_memory = m_screen ? CreateCompatibleDC(m_screen) : nullptr;
        if (!m_memory) {
            return;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        m_bitmap = CreateDIBSection(m_memory, &info, DIB_RGB_COLORS, &m_pixels, nullptr, 0);
        if (m_bitmap) {
            m_previous = SelectObject(m_memory, m_bitmap);
        }
    }

    ~CaptureBitmap()
    {
        if (m_previous && m_previous != HGDI_ERROR) {
            SelectObject(m_memory, m_previous);
        }
        if (m_bitmap) {
            DeleteObject(m_bitmap);
        }
        if (m_memory) {
            DeleteDC(m_memory);
        }
        if (m_screen) {
            ReleaseDC(nullptr, m_screen);
        }
    }

    CaptureBitmap(const CaptureBitmap&) = delete;
    CaptureBitmap& operator=(const CaptureBitmap&) = delete;

    [[nodiscard]] bool valid() const
    {
        return m_pixels && m_previous && m_previous != HGDI_ERROR;
    }

    [[nodiscard]] bool capture(int left, int top, int width, int height) const
    {
        return valid() && BitBlt(m_memory, 0, 0, width, height, m_screen, left, top, SRCCOPY | CAPTUREBLT);
    }

    [[nodiscard]] const void* pixels() const
    {
        return m_pixels;
    }

private:
    HDC m_screen = nullptr;
    HDC m_memory = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previous = nullptr;
    void* m_pixels = nullptr;
};

}  // namespace sidescopes
