#pragma once

/// The platform-dependent scaling policy, kept pure so it can be tested without
/// a window or a display. The impure shells in app.cpp read the sizes from GLFW
/// and hand them here; every decision that differs between macOS and Windows
/// lives in one place with a test that names both cases.
namespace sidescopes {

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
