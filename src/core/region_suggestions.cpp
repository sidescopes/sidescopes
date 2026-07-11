#include "core/region_suggestions.h"

#include <cmath>

namespace sidescopes {
namespace {

// Two regions within this many percent on every edge count as the same
// suggestion.
constexpr double kDuplicateTolerancePercent = 2.0;

bool PracticallyEqual(const RegionOfInterest& a, const RegionOfInterest& b) {
    return std::abs(a.left_percent - b.left_percent) <= kDuplicateTolerancePercent &&
           std::abs(a.top_percent - b.top_percent) <= kDuplicateTolerancePercent &&
           std::abs(a.right_percent - b.right_percent) <= kDuplicateTolerancePercent &&
           std::abs(a.bottom_percent - b.bottom_percent) <= kDuplicateTolerancePercent;
}

}  // namespace

std::vector<SuggestedRegion> BuildRegionSuggestions(const std::vector<WindowRegion>& windows) {
    std::vector<SuggestedRegion> suggestions;
    for (const WindowRegion& window : windows) {
        bool duplicate = false;
        for (const SuggestedRegion& existing : suggestions) {
            if (PracticallyEqual(existing.region, window.region)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        SuggestedRegion suggestion;
        suggestion.region = window.region;
        suggestion.label = window.application;
        suggestions.push_back(std::move(suggestion));
    }
    return suggestions;
}

}  // namespace sidescopes
