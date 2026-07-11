#pragma once

#include <string>
#include <vector>

#include "core/analysis_worker.h"
#include "core/app_region_memory.h"
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

// Merges remembered per-application regions, detector candidates (frame
// pixels), and window rectangles (already in percent) into the picker's
// suggestion list: regions remembered for applications with a window on
// screen first, then confident photo canvases, then windows, deduplicated
// when two entries practically coincide. Order is preserved within each
// group - remembered entries arrive most recent first, detector candidates
// strongest first, windows frontmost first.
std::vector<SuggestedRegion> BuildRegionSuggestions(
    const std::vector<RegionCandidate>& photo_candidates, int frame_width, int frame_height,
    const std::vector<WindowRegion>& windows, const std::vector<RememberedRegion>& remembered = {});

}  // namespace sidescopes
