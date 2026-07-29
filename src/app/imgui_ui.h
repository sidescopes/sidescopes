#pragma once

#include "imgui.h"

namespace sidescopes {

/// A tooltip that wraps rather than running off the edge. Without multi-viewport
/// support a tooltip cannot spill past the application window, and this one is
/// often deliberately narrow, so a long line would simply be cut; wrapping
/// tracks the window when that is the tighter of the two. Attaches to the item
/// drawn immediately before the call.
void wrappedTooltip(const char* text);

/// Points the toolkit's error hook at the diagnostic log, and decides whether
/// it also puts its own error window on screen.
///
/// THE LOG IS UNCONDITIONAL. A recording from a machine nobody here can see is
/// the only way one of these ever reaches us, and that path is the whole point
/// of the hook - gating it with the window would quietly remove it.
///
/// The WINDOW is for development alone. A toolkit error is a developer's bug,
/// not a user's: showing it to a user gives them something alarming they can
/// do nothing about, where recording it gives us something we can act on when
/// they send a log. The two are not one message rendered twice - they have
/// different audiences.
void installInterfaceErrorReporting();

/// @return Whether the toolkit's error hook currently points at the diagnostic
///         log rather than at nothing or at somebody else's handler.
///
/// Nothing in the application asks; it exists so a test can tell the hook
/// being installed from merely being non-null. Under the test engine it is
/// never null - the engine installs a handler of its own around every test -
/// so a test that only checked for null would agree with the hook having been
/// gated away, which is the one mistake worth guarding here.
[[nodiscard]] bool interfaceErrorReportingInstalled();

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
