#pragma once

#include <cstdint>
#include <optional>

#include "core/analysis_worker.h"

namespace sidescopes {

// Screenshot-style drag selection over the captured display: dims the
// screen, punches the dragged rectangle clear, ESC cancels. Blocking; the
// capture and analysis threads keep running underneath. Returns the region
// as percentages of the display, or nothing on cancel.
std::optional<RegionOfInterest> PickRegionOnDisplay(uint32_t display_id);

// Persistent, click-through border around the monitored region so users can
// see what the scopes are reading. Drawn OUTSIDE the region so the border
// itself never enters the scoped pixels. Idempotent: call Show again to
// move it.
void ShowRegionBorder(uint32_t display_id, const RegionOfInterest& region);
void HideRegionBorder();

}  // namespace sidescopes
