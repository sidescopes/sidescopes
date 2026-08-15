#pragma once

namespace sidescopes {

/// The application's window keeps a fixed share; the picture - which stands
/// in for the screen it would be reading - takes the rest. Side by side on a
/// wide viewport, stacked on a tall one, and never inside the application's
/// own window: on a desktop the two are separate windows, and a demo that
/// nests one in the other teaches the wrong shape.
constexpr float AppWidthPoints = 400.0f;
constexpr float AppHeightShare = 0.52f;
constexpr float WideEnough = 1.05f;
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

struct ShellLayout
{
    LayoutPoint screenPos;
    LayoutPoint screenSize;
    LayoutPoint appPos;
    LayoutPoint appSize;
};

/// The rule itself, kept where it can be TESTED. It decides the one thing
/// about this demo that would teach something false if it were wrong: the
/// application is a window BESIDE the picture, never a frame around it,
/// because beside is what it is on a desktop.
[[nodiscard]] ShellLayout layoutFor(const LayoutPoint& origin, const LayoutPoint& size);

}  // namespace sidescopes
