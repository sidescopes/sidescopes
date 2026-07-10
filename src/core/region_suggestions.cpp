#include "core/region_suggestions.h"

#include <cmath>

namespace sidescopes {
namespace {

// Detector candidates below this confidence are noise, not suggestions.
constexpr float kMinimumConfidence = 0.35f;

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

std::vector<SuggestedRegion> BuildRegionSuggestions(
    const std::vector<RegionCandidate>& photo_candidates, int frame_width, int frame_height,
    const std::vector<WindowRegion>& windows) {
    std::vector<SuggestedRegion> suggestions;
    if (frame_width <= 0 || frame_height <= 0) return suggestions;

    for (const RegionCandidate& candidate : photo_candidates) {
        if (candidate.confidence < kMinimumConfidence) continue;
        SuggestedRegion suggestion;
        suggestion.region.left_percent = candidate.rect.x * 100.0 / frame_width;
        suggestion.region.top_percent = candidate.rect.y * 100.0 / frame_height;
        suggestion.region.right_percent =
            (candidate.rect.x + candidate.rect.width) * 100.0 / frame_width;
        suggestion.region.bottom_percent =
            (candidate.rect.y + candidate.rect.height) * 100.0 / frame_height;
        suggestion.label = "Area";
        suggestions.push_back(std::move(suggestion));
    }

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
