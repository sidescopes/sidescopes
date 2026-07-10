#pragma once

#include <string>
#include <vector>

#include "core/analysis_worker.h"
#include "core/photo_region_detector.h"

namespace sidescopes {

// A region the picker offers for one-click confirmation.
struct SuggestedRegion {
    RegionOfInterest region;
    std::string label;
};

// A window rectangle in percent of the display, with its owning application.
struct WindowRegion {
    RegionOfInterest region;
    std::string application;
};

// Merges detector candidates (frame pixels) and window rectangles (already
// in percent) into the picker's suggestion list: confident photo canvases
// first, then windows, deduplicated when a canvas practically fills its
// window. Order is preserved within each group - detector candidates arrive
// strongest first, windows frontmost first.
std::vector<SuggestedRegion> BuildRegionSuggestions(
    const std::vector<RegionCandidate>& photo_candidates, int frame_width, int frame_height,
    const std::vector<WindowRegion>& windows);

}  // namespace sidescopes
