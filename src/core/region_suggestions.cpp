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

std::vector<SuggestedRegion> BuildFaceSuggestions(const std::vector<IntRect>& faces,
                                                  int frame_width, int frame_height) {
    // The inward shrink per side: enough to shed hair and background at
    // the box edges without losing the cheeks.
    constexpr double kInsetFraction = 0.1;

    std::vector<SuggestedRegion> suggestions;
    if (frame_width <= 0 || frame_height <= 0) return suggestions;
    for (const IntRect& face : faces) {
        const double inset_x = face.width * kInsetFraction;
        const double inset_y = face.height * kInsetFraction;
        SuggestedRegion suggestion;
        suggestion.region.left_percent = (face.x + inset_x) * 100.0 / frame_width;
        suggestion.region.top_percent = (face.y + inset_y) * 100.0 / frame_height;
        suggestion.region.right_percent = (face.x + face.width - inset_x) * 100.0 / frame_width;
        suggestion.region.bottom_percent = (face.y + face.height - inset_y) * 100.0 / frame_height;
        suggestion.label = "Face";
        bool duplicate = false;
        for (const SuggestedRegion& existing : suggestions) {
            if (PracticallyEqual(existing.region, suggestion.region)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) suggestions.push_back(std::move(suggestion));
    }
    return suggestions;
}

}  // namespace sidescopes
