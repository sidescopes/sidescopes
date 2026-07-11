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

// Merges detected faces and photo canvases (frame pixels) and window
// rectangles (already in percent) into the picker's suggestion list, in
// that order: semantic detection beats geometry. Deduplicated when two
// entries practically coincide. Order is preserved within each group -
// faces and detector candidates largest or strongest first, windows
// frontmost first.
std::vector<SuggestedRegion> BuildRegionSuggestions(
    const std::vector<RegionCandidate>& photo_candidates, int frame_width, int frame_height,
    const std::vector<WindowRegion>& windows, const std::vector<IntRect>& faces = {});

}  // namespace sidescopes
