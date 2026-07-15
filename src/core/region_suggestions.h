#pragma once

#include <string>
#include <vector>

#include "core/analysis_worker.h"

namespace sidescopes {

// A region the picker offers for one-click confirmation.
struct SuggestedRegion
{
    RegionOfInterest region;
    std::string label;
};

// A window rectangle in percent of the display, with its owning application.
struct WindowRegion
{
    RegionOfInterest region;
    std::string application;
};

// The picker's suggestion list: the visible application windows, frontmost
// first, deduplicated when two practically coincide. Window rectangles come
// from the operating system and are exact - the picker offers nothing it
// could be wrong about.
std::vector<SuggestedRegion> buildRegionSuggestions(const std::vector<WindowRegion>& windows);

// Face rectangles (frame pixels) as picker suggestions. The detector's box
// is shrunk inward: scoping a face means judging skin, and edge pixels are
// hair and background that skew the vectorscope's skin cluster.
std::vector<SuggestedRegion> buildFaceSuggestions(const std::vector<IntRect>& faces, int frameWidth, int frameHeight);

}  // namespace sidescopes
