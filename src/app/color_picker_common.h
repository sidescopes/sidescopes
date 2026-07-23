#pragma once

#include <cstddef>

#include "app/pin_board.h"
#include "core/frame.h"
#include "imgui.h"

namespace sidescopes {

// Tooltip strings shared by the deck header, its rows, and the difference row
// under the hero. One line per column: a paragraph covering all three tells a
// reader about two quantities they did not ask about. The sign gloss is the
// convention every colorimetric tool follows; hue has none, because it runs
// around a circle rather than along an axis.
inline constexpr const char* PickerDeltaETip =
    "CIEDE2000 difference from the live color, lower is closer (sRGB assumed)";
inline constexpr const char* PickerLchTips[3] = {
    "how much lighter (+) or darker (-) the live color is",
    "how much more colorful (+) or duller (-) the live color is",
    "how far the live color's hue has drifted - counts for less when the color is dull",
};
inline constexpr const char* PickerRgbTips[3] = {
    "how much more (+) or less (-) red the live color is",
    "how much more (+) or less (-) green the live color is",
    "how much more (+) or less (-) blue the live color is",
};

// The live color, its formatted values, and the shared column metrics every
// picker section measures against. pins is borrowed for the length of one draw.
struct PickerContext
{
    FloatColor color;
    PinBoard& pins;
    ImFont* monospaceFont;
    float labelColumn;
    float percentColumn;
    float columnGap;
    float channelStride;
    float hexWidth;
    float lineHeight;
    const char* hex;
};

/// @return @p source as the opaque swatch color a color button takes.
[[nodiscard]] ImVec4 pickerSwatchColor(const FloatColor& source);

/// Writes the hex code of the pin at @p index into @p buffer, which holds the
/// eight bytes a code and its terminator need.
void pinHexOf(const PinBoard& pins, std::size_t index, char* buffer);

/// The CIEDE2000 distance itself, one decimal the way the field quotes it. It
/// replaced a friendlier "match percentage": that read as a claim about how
/// alike two colors LOOK, which the measure cannot honor at the distances this
/// picker works over - it is built and validated for small differences.
void formatDeltaE(float deltaE, char (&value)[8]);

}  // namespace sidescopes
