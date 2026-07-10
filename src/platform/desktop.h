#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sidescopes {

// Small desktop services the app needs outside capture itself.

struct DesktopPoint {
    double x = 0.0;
    double y = 0.0;
};

struct DisplayGeometry {
    double origin_x = 0.0;  // global desktop coordinates, top-left origin
    double origin_y = 0.0;
    double width_points = 0.0;
    double height_points = 0.0;
};

// Global cursor position in desktop points. Reading the position requires no
// special permission on any supported platform.
std::optional<DesktopPoint> GlobalCursorPosition();

std::optional<DisplayGeometry> GeometryOfDisplay(uint32_t display_id);

// Preferences file location in the platform's convention.
std::string PreferencesFilePath();

}  // namespace sidescopes
