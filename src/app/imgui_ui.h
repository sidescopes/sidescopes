#pragma once

#include "imgui.h"

namespace sidescopes {

/// A tooltip that wraps rather than running off the edge. Without multi-viewport
/// support a tooltip cannot spill past the application window, and this one is
/// often deliberately narrow, so a long line would simply be cut; wrapping
/// tracks the window when that is the tighter of the two. Attaches to the item
/// drawn immediately before the call.
void wrappedTooltip(const char* text);

/// A tool drawn as an icon glyph in a square button: the region tools above the
/// panes and the pin tool in the status bar. @p dimmed draws the glyph faint,
/// for a tool that is standing down. Returns true when the button is pressed.
bool iconButton(const char* id, ImTextureID texture, const char* tooltip, bool dimmed = false);

/// The same button carrying @p label beside its glyph, for a control that has
/// a short value to show as well as an identity - the preset picker, which
/// names the slot it is on. It stands beside a plain @ref iconButton as a
/// sibling: one height, one glyph size, one hover box, wider only by the value.
///
/// @p labelWidth is the width RESERVED for the label, measured from the widest
/// one the button will ever carry, and the label is drawn left-aligned in it,
/// so a character coming or going moves nothing on the row. Returns true when
/// the button is pressed.
bool labelledIconButton(const char* id, ImTextureID texture, const char* label, float labelWidth, const char* tooltip);

}  // namespace sidescopes
