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

// One poll of the asynchronous picker. `preview` is whatever the user is
// currently indicating - the hovered suggestion, the drag in progress, or
// the adjusted rectangle - so the application can feed it to the scopes
// live. On the poll that delivers `finished`, `confirmed` carries the
// chosen region, or nothing on cancel, and the overlay is gone.
struct RegionPickPoll {
    bool active = false;
    bool finished = false;
    std::optional<RegionOfInterest> preview;
    std::optional<RegionOfInterest> confirmed;
};

// Screenshot-style selection over the captured display. Pick mode
// highlights the detected rectangle (photo canvases and windows) under the
// cursor with the system accent for one-click confirmation, the way the
// macOS screenshot interface selects windows; draw mode dims the screen
// for dragging an area by hand. `current_region` seeds draw mode with the
// present selection so it can be moved, resized by its handles, or
// confirmed as-is; pass nothing when the whole screen is scoped. ESC
// cancels.
//
// The picker is asynchronous: Begin shows the overlay and returns, the
// application keeps running its frame loop (which also pumps the
// overlay's events), and PollRegionPick is read once per frame so the
// scopes can preview the selection before it is confirmed.
bool BeginRegionPick(uint32_t display_id, const std::vector<SuggestedRegion>& suggestions,
                     const std::optional<RegionOfInterest>& current_region,
                     RegionPickerMode initial_mode);
RegionPickPoll PollRegionPick();

// Persistent, click-through border around the monitored region so users can
// see what the scopes are reading. Drawn OUTSIDE the region so the border
// itself never enters the scoped pixels. Idempotent: call Show again to
// move it.
void ShowRegionBorder(uint32_t display_id, const RegionOfInterest& region);
void HideRegionBorder();

}  // namespace sidescopes
