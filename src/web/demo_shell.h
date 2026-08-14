#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/shortcut_resolver.h"
#include "core/frame.h"
#include "imgui.h"
#include "sidescopes/module.h"
#include "web/region_editor.h"

namespace sidescopes {

class ScopeView;
class ScopeRegistry;

/// @brief The keyboard, and the colour under the pointer.
///
/// The two pieces of the application shell that the demo has to supply
/// itself, because the desktop takes both from the operating system: keys
/// arrive through GLFW callbacks the shell installs, and the colour comes
/// from a screen sample. Neither exists here in that form — the keys come
/// from Dear ImGui, and the "screen" is a picture in a pane.
///
/// The MAPPING is not rebuilt. ShortcutResolver owns which key means what,
/// and this only answers "is that key down" and applies what it returns, so
/// the demo's keyboard cannot drift from the application's.
namespace shell {

/// Whether the named binding went down this frame. Names are what a binding
/// holds: a letter A-Z, "Escape", a digit "1"-"9", or "Comma".
[[nodiscard]] bool keyPressed(std::string_view name);

/// The live modifier state, as the resolver reads it.
[[nodiscard]] ModifierState modifiers();

/// The colour of the picture under @p point, or nothing when the pointer is
/// outside it. The desktop samples the screen here; the demo samples the
/// only picture it has.
[[nodiscard]] std::optional<FloatColor> sampleAt(const ImVec2& point, const RegionEditor::Placement& placement,
                                                 const std::vector<uint8_t>& rgba, int width, int height);

/// The average colour over @p rect of the picture, which is what the pin
/// tool takes when it is dragged rather than clicked: one pixel of skin,
/// sky or fabric is very often unrepresentative, so the application
/// averages an area and so does this.
[[nodiscard]] std::optional<FloatColor> averageOver(const SsRect& rect, const std::vector<uint8_t>& rgba, int width,
                                                    int height);

/// Sets the canvas's own cursor. Dear ImGui has no crosshair in its cursor
/// enum, and the region tools want one, so this reaches past it to the CSS
/// the page already understands. Repeats are free: the value is remembered
/// and only a change crosses into JavaScript.
///
/// Pass nullptr to hand the cursor back to Dear ImGui.
void setCanvasCursor(const char* css);

/// The pin tool's cursor: crosshair and preview swatch, in the CURSOR
/// IMAGE itself.
///
/// The same construction the desktop uses, and for the same reason. A swatch
/// painted into a window trails the pointer by a composition frame plus
/// whatever the redraw cadence is - here a 20/s cap, so up to 50 ms, which
/// on a fast movement is a visible gap between the crosshair and the colour.
/// The cursor image rides the compositor's own zero-latency plane, so a
/// swatch drawn into it cannot lag at all.
///
/// The image is rebuilt only when @p colour CHANGES, never when the pointer
/// moves: moving it is the compositor's job. That is what makes this cheap
/// enough to do at all, and it is the desktop's rule too.
void setPinCursor(const std::optional<FloatColor>& colour);

}  // namespace shell

}  // namespace sidescopes
