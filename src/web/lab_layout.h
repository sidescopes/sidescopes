#pragma once

#include <cstdint>
#include <vector>

namespace sidescopes {

/// The application's window keeps a fixed share; the picture - which stands
/// in for the screen it would be reading - takes the rest. Side by side on a
/// wide viewport, stacked on a tall one, and never inside the application's
/// own window: on a desktop the two are separate windows, and a lab that
/// nests one in the other teaches the wrong shape.
constexpr float AppWidthPoints = 400.0f;
constexpr float AppHeightShare = 0.52f;
constexpr float WideEnough = 1.05f;
/// A compact square at the centre of the Lab's unobstructed screen area.
/// Relative to the shorter side so it behaves the same in either layout.
constexpr float StarterRegionShare = 0.34f;
/// The application floats above the workspace rather than sitting in it, so
/// it is drawn inset with an edge and a shadow. Without those it reads as a
/// frame the picture is embedded in, which is the opposite of what it is.
constexpr float AppMargin = 14.0f;

/// Where the picture goes and where the application's window goes: beside
/// each other on a wide viewport, stacked on a tall one. Two rectangles,
/// never nested.
/// A point or a size, in points. NOT Dear ImGui's type, deliberately: this
/// unit is the rule for where the two rectangles go, and a rule worth testing
/// has to compile where the tests run - which is everywhere, without a
/// graphics library. The shell converts at the four call sites that need it.
struct LayoutPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

struct LayoutRect
{
    LayoutPoint position;
    LayoutPoint size;
};

struct ShellLayout
{
    LayoutPoint screenPos;
    LayoutPoint screenSize;
    LayoutPoint appPos;
    LayoutPoint appSize;
};

/// Pixels captured from the Lab's virtual display. The byte order is BGRA,
/// matching every ScreenCaptureSource the analysis pipeline receives.
struct LabDisplayCapture
{
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
};

/// The rule itself, kept where it can be TESTED. It decides the one thing
/// about this lab that would teach something false if it were wrong: the
/// application is a window BESIDE the picture, never a frame around it,
/// because beside is what it is on a desktop.
[[nodiscard]] ShellLayout layoutFor(const LayoutPoint& origin, const LayoutPoint& size);

/// The Lab's initial global region. It is centred in the virtual screen area,
/// not placed relative to the simulated application window, so it begins on
/// the subject and remains at one display position when images change.
[[nodiscard]] LayoutRect starterRegionFor(const ShellLayout& layout);

/// Rasterizes a global region from the Lab's virtual display. Pixels beneath
/// the supplied picture come from @p pictureBgra; everything else is opaque
/// black, which is the virtual desktop the canvas visibly presents.
[[nodiscard]] LabDisplayCapture captureVirtualDisplayRegion(const LayoutRect& desktopRegion,
                                                            const LayoutRect& pictureOnDesktop,
                                                            const LayoutPoint& picturePixels,
                                                            const std::vector<uint8_t>& pictureBgra);

}  // namespace sidescopes
