#pragma once

#include <optional>

#include "app/pin_board.h"
#include "core/frame.h"
#include "imgui.h"

namespace sidescopes {

/// Draws the color picker pane: the live sampled color as a large swatch with
/// its values spelled three ways (0-255, percent, hex), the pinned colors
/// alongside, and - when a pin is chosen as comparator - the CIEDE2000
/// differences from the live color. @p monospaceFont aligns the hex columns
/// when a fixed-width font loaded, and is null otherwise.
void drawColorPicker(const std::optional<FloatColor>& liveColor, PinBoard& pins, ImFont* monospaceFont);

}  // namespace sidescopes
