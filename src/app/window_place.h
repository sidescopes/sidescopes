#pragma once

#include <cstdint>
#include <optional>

#include "core/analysis_worker.h"
#include "core/frame.h"
#include "platform/desktop.h"

namespace sidescopes {

/// The application window's rectangle in the platform's own window units -
/// points on macOS, physical pixels on Windows - which is what the toolkit
/// reports and what the saved placement holds.
struct WindowPlacement
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/// @return A compact first-run application window at the left of @p workArea,
///         vertically centred and clamped to the available rectangle. A saved
///         placement bypasses this; it is only the system-independent policy
///         used before the user has positioned the window.
[[nodiscard]] WindowPlacement starterWindowPlacement(const WindowPlacement& workArea, int windowWidth,
                                                     int windowHeight);

/// @return The display @p window sits on, decided by its centre so a window
///         straddling two belongs to whichever shows more of it.
[[nodiscard]] std::optional<uint32_t> displayUnderWindow(const WindowPlacement& window);

/// A moderate square global region centred on @p display when that does not
/// overlap @p window, otherwise moved into nearby open space. Its side is the
/// shorter physical dimension of the starter rectangle, so it remains square
/// on landscape and portrait displays even though the region is expressed in
/// display percentages. It stays on this display when its global desktop
/// origin is not zero. This is the selection a fresh desktop session starts
/// with so the scopes have a live subject before any region tool is discovered.
[[nodiscard]] RegionOfInterest starterGlobalRegion(const WindowPlacement& window, const DisplayGeometry& display);

/// @p window's own rectangle within @p display, in that display's pixels, with
/// generous margins for the chrome the platform draws around it - the mask
/// analysis leaves out of change detection, so the application's own redraws
/// never re-trigger it.
///
/// Display pixels rather than FRAME pixels: the mask is stated in display
/// pixels so it survives a capture narrowed to part of one. @p displayWidth
/// and @p displayHeight are the display's own extents in pixels, and
/// @p uiScale carries the margins - authored in 100%-scale units like the rest
/// of the interface - up with the monitor's scale.
[[nodiscard]] IntRect selfWindowMask(const WindowPlacement& window, const DisplayGeometry& display, int displayWidth,
                                     int displayHeight, float uiScale);

}  // namespace sidescopes
