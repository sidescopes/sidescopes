#pragma once

#include "imgui.h"

namespace sidescopes {

/// A tooltip that wraps rather than running off the edge. Without multi-viewport
/// support a tooltip cannot spill past the application window, and this one is
/// often deliberately narrow, so a long line would simply be cut; wrapping
/// tracks the window when that is the tighter of the two. Attaches to the item
/// drawn immediately before the call.
void wrappedTooltip(const char* text);

/// A scope toggle drawn as a letter chip: professional tools label scopes with
/// text because no icon language exists for them, and the letters double as the
/// keyboard shortcuts. Returns true when the button is pressed.
bool scopeToggleButton(const char* id, const char* letter, bool enabled, const char* tooltip);

/// A tool drawn as an icon glyph in a square button: the region tools above the
/// panes and the pin tool in the status bar. @p dimmed draws the glyph faint,
/// for a tool that is standing down. Returns true when the button is pressed.
bool iconButton(const char* id, ImTextureID texture, const char* tooltip, bool dimmed = false);

}  // namespace sidescopes
