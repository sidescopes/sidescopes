#pragma once

#include <cstdint>
#include <optional>

#include "core/analysis_worker.h"
#include "core/region_suggestions.h"

namespace sidescopes {

// Screenshot-style selection over the captured display, modeled on the
// macOS screenshot interface: a mode toolbar offers scoping the entire
// screen, picking a detected rectangle (photo canvases and windows,
// highlighted under the cursor for one-click confirmation), or drawing an
// area by hand over the dimmed screen. `current_region` seeds draw mode
// with the present selection so it can be moved, resized by its handles,
// or confirmed as-is; pass nothing when the whole screen is scoped. The
// last used mode is remembered across invocations. ESC cancels. Blocking;
// the capture and analysis threads keep running underneath. Returns the
// region as percentages of the display, or nothing on cancel.
std::optional<RegionOfInterest> PickRegionOnDisplay(
    uint32_t display_id, const std::vector<SuggestedRegion>& suggestions,
    const std::optional<RegionOfInterest>& current_region);

// Persistent, click-through border around the monitored region so users can
// see what the scopes are reading. Drawn OUTSIDE the region so the border
// itself never enters the scoped pixels. Idempotent: call Show again to
// move it.
void ShowRegionBorder(uint32_t display_id, const RegionOfInterest& region);
void HideRegionBorder();

}  // namespace sidescopes
