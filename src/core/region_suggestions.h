#pragma once

#include <string>
#include <vector>

#include "core/analysis_worker.h"

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

// The picker's suggestion list: the visible application windows, frontmost
// first, deduplicated when two practically coincide. Window rectangles come
// from the operating system and are exact - the picker offers nothing it
// could be wrong about.
std::vector<SuggestedRegion> BuildRegionSuggestions(const std::vector<WindowRegion>& windows);

}  // namespace sidescopes
