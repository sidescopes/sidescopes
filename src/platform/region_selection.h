#pragma once

#include <cstdint>
#include <optional>

#include "core/analysis_worker.h"
#include "core/region_suggestions.h"

namespace sidescopes {

// How the region picker starts out. The mode is chosen up front by the
// toolbar button (or key) that opened the picker; inside it, A and D
// switch between picking and drawing.
enum class RegionPickerMode { PickDetected, Draw };

// Screenshot-style selection over the captured display. Pick mode
// highlights the detected rectangle (photo canvases and windows) under the
// cursor with the system accent for one-click confirmation, the way the
// macOS screenshot interface selects windows; draw mode dims the screen
// for dragging an area by hand. `current_region` seeds draw mode with the
// present selection so it can be moved, resized by its handles, or
// confirmed as-is; pass nothing when the whole screen is scoped. ESC
// cancels. Blocking; the capture and analysis threads keep running
// underneath. Returns the region as percentages of the display, or
// nothing on cancel.
std::optional<RegionOfInterest> PickRegionOnDisplay(
    uint32_t display_id, const std::vector<SuggestedRegion>& suggestions,
    const std::optional<RegionOfInterest>& current_region, RegionPickerMode initial_mode);

// Persistent, click-through border around the monitored region so users can
// see what the scopes are reading. Drawn OUTSIDE the region so the border
// itself never enters the scoped pixels. Idempotent: call Show again to
// move it.
void ShowRegionBorder(uint32_t display_id, const RegionOfInterest& region);
void HideRegionBorder();

}  // namespace sidescopes
