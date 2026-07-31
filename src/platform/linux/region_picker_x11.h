#pragma once

#include <cairo/cairo.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/linux/x11_overlay.h"
#include "platform/region_geometry.h"

namespace sidescopes {

/// One display's picker sheet: the fullscreen dimmed overlay the region is
/// dragged on, the Linux sibling of the Windows PickerState. Geometry is kept
/// in overlay-local pixels (the sheet covers exactly its display, so local
/// pixels are display pixels); the seam converts to display percentages at
/// the poll boundary, like the other platforms.
struct PickerX11State
{
    OverlayWindow window;
    /// A snapshot of what this sheet covers, taken before it was mapped and
    /// painted as its backdrop where no compositing manager can blend the
    /// sheet's own transparency. Null on a composited desktop, where the
    /// real screen shows through and stays live.
    cairo_surface_t* backdrop = nullptr;
    uint32_t displayId = 0;
    int originX = 0;  ///< The covered display, root coordinates.
    int originY = 0;
    int width = 0;
    int height = 0;
    bool drawMode = false;
    bool facesMode = false;
    /// Whether this display's face scan has finished. Until it has, face mode
    /// stays silent about absence; once set, an empty face list means the
    /// honest "none found".
    bool facesScanned = false;
    /// The attached draw's clamp: set when a drag starts in window mode over
    /// a suggestion - the drag cannot leave constraintRect (local pixels),
    /// everything outside it dims hard, and the label names the target.
    bool constrained = false;
    LocalRect constraintRect{};
    std::string constraintLabel;
    /// A drag in window mode: draws an attached region within the window
    /// under the drag's start instead of confirming a whole window.
    bool pickDragging = false;
    /// Color pinning: a click reports a point to sample, a drag a rectangle
    /// to average, the region is never touched.
    bool pinMode = false;
    double pinnedPointX = 0.0;
    double pinnedPointY = 0.0;
    LocalRect pinnedSample{};
    bool pinnedIsPoint = false;
    bool pinnedKeepOpen = false;
    bool pinnedReady = false;
    /// The active suggestion list in local pixels with labels - the windows
    /// or the faces, depending on the mode.
    std::vector<std::pair<LocalRect, std::string>> windows;
    std::vector<std::pair<LocalRect, std::string>> faces;
    std::vector<std::pair<LocalRect, std::string>> suggestions;
    int hoveredSuggestion = -1;
    bool dragging = false;
    int dragStartX = 0;
    int dragStartY = 0;
    int dragCurrentX = 0;
    int dragCurrentY = 0;
    bool shiftDown = false;
    bool picked = false;
    bool finished = false;
    LocalRect confirmedRect{};

    PickerX11State() = default;
    PickerX11State(const PickerX11State&) = delete;
    PickerX11State& operator=(const PickerX11State&) = delete;

    /// The sheets are cleared wholesale when a pick ends, so the snapshot a
    /// sheet took is released with it.
    ~PickerX11State()
    {
        if (backdrop != nullptr) {
            cairo_surface_destroy(backdrop);
        }
    }
};

/// The open pickers, one per display; empty when no pick is active. Owned by
/// region_selection.cpp, read by the view.
[[nodiscard]] std::vector<std::unique_ptr<PickerX11State>>& openPickers();

/// Holds the colour under the pointer for the pin cursor's swatch, and puts
/// the rebuilt cursor on every open pin sheet. The application pushes this
/// each frame while a pin is live; the cursor itself is rebuilt only when the
/// rounded colour changes.
void setPickerChipColor(const std::optional<FloatColor>& color);

/// Builds one display's sheet: creates its overlay window, seeds the
/// suggestion lists, grabs the keyboard on the first sheet. Null on failure
/// (no X display reachable).
[[nodiscard]] std::unique_ptr<PickerX11State> createPickerSheet(uint32_t displayId, int originX, int originY, int width,
                                                                int height, bool draw, bool faces, bool pin);

/// Repaints a sheet: dim wash, suggestion boxes and labels, the live drag
/// rectangle, the constraint dress, the mode banner.
void paintPicker(PickerX11State& picker);

/// Switches every open sheet between the pick modes (0 window, 1 draw,
/// 2 faces, 3 pin), refreshing suggestions and repainting.
void switchPickerMode(int mode);

/// Ends the pick as Escape would: no choice, overlays to be torn down by
/// the next poll.
void cancelAllPickerSheets();

}  // namespace sidescopes
