#pragma once

namespace sidescopes {

/// Applies the application's dark theme - the base metrics and the color table
/// - to the current ImGui style. The interface scale is applied on top of it,
/// so this is what every size change rebuilds from.
void applyTheme();

/// Rebuilds the themed style at @p scale from the unscaled base. The style is
/// reset to its defaults, re-themed, then scaled once, so repeated calls never
/// compound: ScaleAllSizes multiplies every size while the theme names only a
/// handful, and its truncation would make the leftover compounding
/// irreversible, so the reset is what keeps the scale landing on unscaled
/// sizes.
void applyInterfaceScale(float scale);

}  // namespace sidescopes
