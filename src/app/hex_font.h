#pragma once

#include "imgui.h"

namespace sidescopes {

// Hex codes render in the fixed-width font when one loaded, so
// every code is the same width and columns anchor exactly.

/// The hex and signed values align in a fixed-width font when the system had
/// one; the font is pushed rather than sized by hand so global UI scale applies
/// once. A null font falls back to the interface font.
void pushHexFont(ImFont* font);

/// Pops what pushHexFont pushed, for the same @p font.
void popHexFont(ImFont* font);

/// @return The width @p text takes in @p font, for a column sized to its
///         widest value.
[[nodiscard]] float hexFontWidth(ImFont* font, const char* text);

/// The monospace face may sit at a size of its own (monospaceFontScale),
/// and ImGui top-aligns the items sharing a line, so a value drawn beside
/// an interface-font label lands on a different baseline. Sinking the
/// value by the ascent difference seats both on the label's baseline; the
/// line keeps its own top, so only this item shifts. Rows that position
/// their text by hand use pushHexFont directly instead.
[[nodiscard]] float hexFontBaselineDrop(ImFont* font);

/// Draws @p text in @p font, seated on the baseline of the interface-font
/// labels sharing its line.
void hexFontText(ImFont* font, const char* text);

}  // namespace sidescopes
