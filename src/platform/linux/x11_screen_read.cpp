// The off-stream screen reads on Linux: sampleScreenColorAsync and
// captureDisplayImage, both through X11's XGetImage on the root window. The
// live capture stream is the reliable pixel source and covers the streamed
// display; these serve the pointer readout on OTHER displays and the face
// scan of a non-streamed display, exactly as their macOS/Windows siblings do.
//
// Honest caveat, the same one globalCursorPosition carries: under XWayland
// XGetImage returns the X server's picture, which reflects X (and XWayland)
// content but not native Wayland windows. It is best-effort - a real value
// for the common case, nothing where the server cannot answer - which is what
// the optional return type is for.

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "platform/desktop.h"
#include "platform/linux/x11_displays.h"
#include "platform/linux/x11_error_guard.h"

namespace sidescopes {
namespace {

/// The overlay/read connection, opened once. Separate from the desktop
/// services' main connection so a blocking image fetch never sits in the
/// middle of a geometry query's reply stream.
Display* readDisplay()
{
    static Display* display = []() {
        // Before the first read, because a rootless server refuses that read
        // and Xlib's default handler answers a refusal by exiting.
        ensureX11ErrorHandler();

        return XOpenDisplay(nullptr);
    }();
    return display;
}

/// A rootless X server - which is what XWayland is on a Wayland session -
/// backs the root window with no readable storage and refuses every read of
/// it. One refusal of a rectangle known to lie inside the root proves the
/// server is that kind, so the frame loop stops asking. Process-wide because
/// it describes the server rather than a caller, atomic because the face-scan
/// threads read the screen too.
std::atomic<bool> g_rootUnreadable{false};

/// One pixel out of an XImage as BGRA bytes. XGetImage on a truecolor visual
/// packs the channels by the visual's masks; shifting by each mask's low bit
/// recovers them without assuming a byte order.
void unpackPixel(const XImage* image, unsigned long pixel, uint8_t& blue, uint8_t& green, uint8_t& red)
{
    const auto channel = [pixel](unsigned long mask) -> uint8_t {
        if (mask == 0) {
            return 0;
        }
        unsigned long value = pixel & mask;
        while ((mask & 1) == 0) {
            mask >>= 1;
            value >>= 1;
        }
        // Scale whatever bit width the channel has up to eight.
        while (mask < 0xFF) {
            mask = (mask << 1) | 1;
            value = (value << 1) | (value & 1);
        }
        return static_cast<uint8_t>(value & 0xFF);
    };
    red = channel(image->red_mask);
    green = channel(image->green_mask);
    blue = channel(image->blue_mask);
}

}  // namespace

void sampleScreenColorAsync(DesktopPoint point, std::function<void(std::optional<FloatColor>)> callback)
{
    const std::function<void(std::optional<FloatColor>)> reader = std::move(callback);
    Display* display = readDisplay();
    if (display == nullptr || g_rootUnreadable.load(std::memory_order_relaxed)) {
        reader(std::nullopt);
        return;
    }
    // A small neighbourhood around the point, clamped to the root, averaged -
    // the same shape the other platforms sample.
    constexpr int Side = 5;
    const int rootWidth = DisplayWidth(display, DefaultScreen(display));
    const int rootHeight = DisplayHeight(display, DefaultScreen(display));
    const int x = std::clamp(static_cast<int>(point.x) - Side / 2, 0, std::max(rootWidth - Side, 0));
    const int y = std::clamp(static_cast<int>(point.y) - Side / 2, 0, std::max(rootHeight - Side, 0));
    XImage* image = XGetImage(display, DefaultRootWindow(display), x, y, Side, Side, AllPlanes, ZPixmap);
    if (image == nullptr) {
        if (rootWidth >= Side && rootHeight >= Side) {
            g_rootUnreadable.store(true, std::memory_order_relaxed);
        }
        reader(std::nullopt);
        return;
    }
    double sumR = 0.0;
    double sumG = 0.0;
    double sumB = 0.0;
    for (int py = 0; py < Side; ++py) {
        for (int px = 0; px < Side; ++px) {
            uint8_t b = 0;
            uint8_t g = 0;
            uint8_t r = 0;
            unpackPixel(image, XGetPixel(image, px, py), b, g, r);
            sumR += r;
            sumG += g;
            sumB += b;
        }
    }
    XDestroyImage(image);
    const double count = Side * Side;
    reader(FloatColor{static_cast<float>(sumR / count), static_cast<float>(sumG / count),
                      static_cast<float>(sumB / count)});
}

std::optional<CapturedImage> captureDisplayImage(uint32_t displayId)
{
    Display* display = readDisplay();
    if (display == nullptr || g_rootUnreadable.load(std::memory_order_relaxed)) {
        return std::nullopt;
    }
    std::optional<DisplayGeometry> geometry;
    for (const LinuxDisplay& candidate : connectedDisplays()) {
        if (candidate.id == displayId) {
            geometry = candidate.geometry;
        }
    }
    if (!geometry || geometry->widthPoints <= 0 || geometry->heightPoints <= 0) {
        return std::nullopt;
    }
    const int width = static_cast<int>(geometry->widthPoints);
    const int height = static_cast<int>(geometry->heightPoints);
    XImage* image = XGetImage(display, DefaultRootWindow(display), static_cast<int>(geometry->originX),
                              static_cast<int>(geometry->originY), width, height, AllPlanes, ZPixmap);
    if (image == nullptr) {
        return std::nullopt;
    }
    CapturedImage captured;
    captured.width = width;
    captured.height = height;
    captured.bgra.resize(static_cast<std::size_t>(width) * height * 4);
    for (int py = 0; py < height; ++py) {
        uint8_t* row = captured.bgra.data() + static_cast<std::size_t>(py) * width * 4;
        for (int px = 0; px < width; ++px) {
            uint8_t* out = row + static_cast<std::size_t>(px) * 4;
            unpackPixel(image, XGetPixel(image, px, py), out[0], out[1], out[2]);
            out[3] = 255;
        }
    }
    XDestroyImage(image);

    return captured;
}

}  // namespace sidescopes
