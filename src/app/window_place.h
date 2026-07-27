#pragma once

#include <cstdint>
#include <optional>

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

/// @return The display @p window sits on, decided by its centre so a window
///         straddling two belongs to whichever shows more of it.
[[nodiscard]] std::optional<uint32_t> displayUnderWindow(const WindowPlacement& window);

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
