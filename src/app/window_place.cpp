#include "app/window_place.h"

namespace sidescopes {
namespace {

// The chrome the platform draws around the window, in 100%-scale units: the
// margins are deliberately generous, since a mask a little too large costs a
// band of screen nobody is scoping and a mask too small costs a redraw loop.
constexpr float MarginLeft = 8.0f;
constexpr float MarginTop = 42.0f;
constexpr float MarginWidth = 16.0f;
constexpr float MarginHeight = 58.0f;

}  // namespace

std::optional<uint32_t> displayUnderWindow(const WindowPlacement& window)
{
    return displayAtPoint(DesktopPoint{window.x + window.width / 2.0, window.y + window.height / 2.0});
}

IntRect selfWindowMask(const WindowPlacement& window, const DisplayGeometry& display, int displayWidth,
                       int displayHeight, float uiScale)
{
    const double scaleX = displayWidth / display.widthPoints;
    const double scaleY = displayHeight / display.heightPoints;

    return IntRect{static_cast<int>((window.x - display.originX - MarginLeft * uiScale) * scaleX),
                   static_cast<int>((window.y - display.originY - MarginTop * uiScale) * scaleY),
                   static_cast<int>((static_cast<float>(window.width) + MarginWidth * uiScale) * scaleX),
                   static_cast<int>((static_cast<float>(window.height) + MarginHeight * uiScale) * scaleY)};
}

}  // namespace sidescopes
