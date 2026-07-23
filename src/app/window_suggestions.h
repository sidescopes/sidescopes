#pragma once

#include <vector>

#include "core/region_suggestions.h"
#include "platform/desktop.h"

namespace sidescopes {

/// The pick suggestions for one display, built from its on-screen windows.
/// @p windows is that display's windows frontmost first (as the window server
/// reports them), in global desktop points; @p geometry places the display in
/// the same space so the rectangles become display-relative percentages;
/// @p maxSuggestions caps how many are suggested.
///
/// A window living mostly inside a larger window of the same application is
/// auxiliary chrome - an editor draws its panels and info overlays as
/// borderless windows over the main one - so it is never suggested and never
/// occludes: a panel over the photo cannot disqualify the photo. Of the rest,
/// only the windows still at least MinimumVisibleFraction visible under the
/// union of the windows in front of them are kept, so a window buried behind
/// the ones on top drops out. Survivors stay frontmost first, the topmost
/// suggested first.
[[nodiscard]] std::vector<SuggestedRegion> buildWindowSuggestions(const std::vector<DesktopWindow>& windows,
                                                                  const DisplayGeometry& geometry, int maxSuggestions);

/// A window rectangle as its display's percentages - the shape the
/// window-candidate list and the edit-time veil speak. Clamped to the
/// display, so a window straddling an edge reports the visible part.
[[nodiscard]] RegionOfInterest displayPercentRect(const WindowGeometry& windowGeom, const DisplayGeometry& display);

}  // namespace sidescopes
