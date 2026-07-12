#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/frame.h"

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

// An on-screen window, front-to-back, in global desktop points (top-left
// origin). Only ordinary application windows are reported - no menu bar,
// dock, or overlay layers - and never this application's own windows.
struct DesktopWindow {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    std::string application;
};

// Windows currently visible on the given display, frontmost first. Window
// geometry comes from the window server and needs no capture permission.
std::vector<DesktopWindow> OnScreenWindows(uint32_t display_id);

// Global cursor position in desktop points. Reading the position requires no
// special permission on any supported platform.
std::optional<DesktopPoint> GlobalCursorPosition();

std::optional<DisplayGeometry> GeometryOfDisplay(uint32_t display_id);

// The display containing the given desktop point, if any.
std::optional<uint32_t> DisplayAtPoint(DesktopPoint point);

// The display the cursor is on right now. Photographers park the cursor
// near the editor, so this is where region picking should open.
std::optional<uint32_t> DisplayUnderCursor();

// Preferences file location in the platform's convention.
std::string PreferencesFilePath();

// Opens the operating system's screen-recording permission pane, as deep
// as the platform allows. A no-op where capture needs no permission.
void OpenScreenRecordingSettings();

// The keyboard modifiers as the operating system sees them right now.
// System overlays (the macOS screenshot interface, for one) swallow
// key-up events, leaving event-driven modifier tracking stuck; polling
// the ground truth lets the application heal itself.
struct ModifierState {
    bool shift = false;
    bool control = false;
    bool option = false;
    bool command = false;
};
ModifierState CurrentModifiers();

// Absolute paths of system fonts suitable for the interface, best first;
// the application loads the first one that exists.
std::vector<std::string> InterfaceFontFiles();

// Invokes the callback - on the platform's main-thread event context -
// whenever the display or the user session wakes up. Capture streams
// survive those transitions as zombies that look alive but deliver
// nothing, so the application restarts capture on the signal. A no-op on
// platforms whose capture dies loudly instead (Windows duplication
// reports access loss and the retry loop rebuilds it).
void ObserveSystemWake(std::function<void()> callback);

// Invokes the callback when Escape is pressed while this application is
// active but no window of it has keyboard focus. The region border never
// takes focus - grabbing it must not pull the keyboard out of the editor
// - so a border interaction can leave the application active yet deaf to
// its own Escape shortcut; this closes that gap. A no-op on platforms
// where the gap cannot occur (on Windows the border never activates the
// application in the first place).
void ObserveEscapeWithoutKeyWindow(std::function<void()> callback);

// Samples the averaged screen color around a desktop point on whatever
// display it falls on, independent of the capture stream, so the cursor
// readout works on every screen and not only the tracked one. This
// application's own windows are excluded where the platform allows.
// Asynchronous: the callback may fire on any thread, or synchronously
// where reading the screen is immediate; it receives nothing when the
// point cannot be read. Callers own the pacing.
void SampleScreenColorAsync(DesktopPoint point,
                            std::function<void(std::optional<FloatColor>)> callback);

}  // namespace sidescopes
