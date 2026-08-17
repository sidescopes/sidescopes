#include "web/lab_layout.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

namespace {

constexpr float MaxCaptureScale = 4.0f;

float captureScaleFor(const LayoutRect& pictureOnDesktop, int pictureWidth, int pictureHeight)
{
    if (pictureWidth <= 0 || pictureHeight <= 0 || pictureOnDesktop.size.x <= 0.0f || pictureOnDesktop.size.y <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(std::max(static_cast<float>(pictureWidth) / pictureOnDesktop.size.x,
                               static_cast<float>(pictureHeight) / pictureOnDesktop.size.y),
                      1.0f, MaxCaptureScale);
}

void makeOpaqueBlack(LabDisplayCapture& capture)
{
    capture.bgra.assign(static_cast<std::size_t>(capture.width) * capture.height * 4u, 0);
    for (std::size_t pixel = 3; pixel < capture.bgra.size(); pixel += 4) {
        capture.bgra[pixel] = 255;
    }
}

void copyPicturePixels(LabDisplayCapture& capture, const LayoutRect& desktopRegion, const LayoutRect& pictureOnDesktop,
                       int pictureWidth, int pictureHeight, const std::vector<uint8_t>& pictureBgra)
{
    for (int y = 0; y < capture.height; ++y) {
        const float displayY = desktopRegion.position.y + (static_cast<float>(y) + 0.5f) * desktopRegion.size.y /
                                                              static_cast<float>(capture.height);
        const float sourceY =
            (displayY - pictureOnDesktop.position.y) / pictureOnDesktop.size.y * static_cast<float>(pictureHeight);
        if (sourceY < 0.0f || sourceY >= static_cast<float>(pictureHeight)) {
            continue;
        }
        const int sy = std::min(pictureHeight - 1, static_cast<int>(sourceY));
        for (int x = 0; x < capture.width; ++x) {
            const float displayX = desktopRegion.position.x + (static_cast<float>(x) + 0.5f) * desktopRegion.size.x /
                                                                  static_cast<float>(capture.width);
            const float sourceX =
                (displayX - pictureOnDesktop.position.x) / pictureOnDesktop.size.x * static_cast<float>(pictureWidth);
            if (sourceX < 0.0f || sourceX >= static_cast<float>(pictureWidth)) {
                continue;
            }
            const int sx = std::min(pictureWidth - 1, static_cast<int>(sourceX));
            const std::size_t source = (static_cast<std::size_t>(sy) * pictureWidth + sx) * 4u;
            const std::size_t target = (static_cast<std::size_t>(y) * capture.width + x) * 4u;
            std::copy_n(pictureBgra.data() + source, 4u, capture.bgra.data() + target);
        }
    }
}

}  // namespace

ShellLayout layoutFor(const LayoutPoint& origin, const LayoutPoint& size)
{
    if (size.x >= size.y * WideEnough) {
        const float appWidth = std::min(AppWidthPoints, size.x * 0.5f);

        return ShellLayout{origin, LayoutPoint{size.x - appWidth, size.y},
                           LayoutPoint{origin.x + size.x - appWidth + AppMargin, origin.y + AppMargin},
                           LayoutPoint{appWidth - AppMargin * 2.0f, size.y - AppMargin * 2.0f}};
    }
    const float appHeight = size.y * AppHeightShare;

    return ShellLayout{origin, LayoutPoint{size.x, size.y - appHeight},
                       LayoutPoint{origin.x + AppMargin, origin.y + size.y - appHeight + AppMargin},
                       LayoutPoint{size.x - AppMargin * 2.0f, appHeight - AppMargin * 2.0f}};
}

LayoutRect starterRegionFor(const ShellLayout& layout)
{
    const float side = std::max(0.0f, std::min(layout.screenSize.x, layout.screenSize.y) * StarterRegionShare);

    return LayoutRect{LayoutPoint{layout.screenPos.x + (layout.screenSize.x - side) * 0.5f,
                                  layout.screenPos.y + (layout.screenSize.y - side) * 0.5f},
                      LayoutPoint{side, side}};
}

LabDisplayCapture captureVirtualDisplayRegion(const LayoutRect& desktopRegion, const LayoutRect& pictureOnDesktop,
                                              const LayoutPoint& picturePixels, const std::vector<uint8_t>& pictureBgra)
{
    LabDisplayCapture capture;
    const int pictureWidth = std::max(0, static_cast<int>(std::lround(picturePixels.x)));
    const int pictureHeight = std::max(0, static_cast<int>(std::lround(picturePixels.y)));
    // Fitting a photograph into the canvas must not quietly lower the scope
    // input to one sample per CSS point. Keep the photograph's effective
    // source density, including across the black part of the same display
    // capture. Four samples per point is already beyond common display
    // density and bounds the extra frame when somebody loads a huge image.
    const float captureScale = captureScaleFor(pictureOnDesktop, pictureWidth, pictureHeight);
    capture.width = std::max(0, static_cast<int>(std::lround(desktopRegion.size.x * captureScale)));
    capture.height = std::max(0, static_cast<int>(std::lround(desktopRegion.size.y * captureScale)));
    if (capture.width == 0 || capture.height == 0) {
        return capture;
    }

    // Opaque black is part of the measured display, not transparency. Alpha
    // is ignored by the current scopes but stamping it makes this a valid
    // captured frame for every consumer at the platform seam.
    makeOpaqueBlack(capture);
    const std::size_t pictureBytes = static_cast<std::size_t>(pictureWidth) * pictureHeight * 4u;
    if (pictureWidth == 0 || pictureHeight == 0 || pictureOnDesktop.size.x <= 0.0f || pictureOnDesktop.size.y <= 0.0f ||
        pictureBgra.size() < pictureBytes) {
        return capture;
    }

    // Sample at destination-pixel centres. This is the same mapping the GPU
    // uses to fit the photograph into the virtual display, retained at source
    // density so the waveform does not lose detail merely because the canvas
    // is smaller than the supplied image.
    copyPicturePixels(capture, desktopRegion, pictureOnDesktop, pictureWidth, pictureHeight, pictureBgra);

    return capture;
}

}  // namespace sidescopes
