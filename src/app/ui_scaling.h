#pragma once

#include <array>

/// The platform-dependent scaling policy, kept pure so it can be tested without
/// a window or a display. The impure shells in app.cpp read the sizes from GLFW
/// and hand them here; every decision that differs between macOS and Windows
/// lives in one place with a test that names both cases.
namespace sidescopes {

/// The interface font's base point size, before any scaling. The scaling steps
/// act on this, and the test that they stay visibly distinct references it.
inline constexpr float InterfaceFontSize = 13.0f;

/// The interface-size steps the menu offers, ascending: multipliers on the OS
/// scale, so 1.0 is "the system's own scaling unchanged" - its per-monitor
/// recommendation is already the baseline. Steps below shrink the chrome (a
/// companion tool wants to be compact beside an editor); steps above enlarge it.
///
/// Discrete on purpose: ImGui rounds the baked font size, so only a fixed set
/// keeps the font and the layout metrics close and each step a visibly distinct
/// size. A free slider would let them disagree at arbitrary points and seat
/// glyphs on half pixels - the defect the row-seating invariants exist to catch.
inline constexpr std::array<float, 7> UiScaleSteps = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};

/// Snaps a stored or requested factor to the nearest offered step, and to 1.0
/// (match system) for anything out of range or unparseable - the file may be
/// hand-edited.
[[nodiscard]] float cleanedUiScaleFactor(float requested);

/// The rasterizer density the interface font should bake at.
///
/// ImGui multiplies this into the framebuffer scale it derives each frame, so
/// on a display whose framebuffer exceeds its window in pixels - a Retina panel
/// reporting @p framebufferWidthPx twice @p windowWidthPx - the extra factor
/// supersamples the glyph and text is measurably crisper. Where the two match,
/// as on Windows where the window is sized in physical pixels at any DPI, the
/// density stays 1.0: asking for more there only bakes larger than the drawn
/// box for no gain.
///
/// Removing this supersample on Retina is exactly the regression b824e9d
/// shipped; the test on this function is the guard against it recurring.
[[nodiscard]] float interfaceFontDensity(int windowWidthPx, int framebufferWidthPx);

/// The factor the interface is scaled by so it occupies the same physical size
/// on every display.
///
/// The UI is authored in 100%-scale units. On macOS the window is measured in
/// logical points and the framebuffer alone carries the Retina factor, so the
/// scale is 1.0; on Windows the window is sized in physical pixels and
/// @p contentScale (1.25, 1.5, ...) is what says how many of them one unit
/// should cover. The framebuffer-to-window ratio distinguishes the two.
[[nodiscard]] float uiScaleForWindow(float contentScale, int windowWidthPx, int framebufferWidthPx);

}  // namespace sidescopes
