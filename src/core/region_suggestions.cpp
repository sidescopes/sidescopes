#include "core/region_suggestions.h"

#include <algorithm>
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
    const std::vector<WindowRegion>& windows, const std::vector<RememberedRegion>& remembered) {
    std::vector<SuggestedRegion> suggestions;
    if (frame_width <= 0 || frame_height <= 0) return suggestions;

    // Remembered regions surface only while their application has a window
    // on screen: a suggestion for an editor that is not even running is
    // noise.
    for (const RememberedRegion& entry : remembered) {
        const bool application_visible = std::any_of(
            windows.begin(), windows.end(),
            [&](const WindowRegion& window) { return window.application == entry.application; });
        if (!application_visible) continue;
        SuggestedRegion suggestion;
        suggestion.region = entry.region;
        suggestion.label = entry.application + " (remembered)";
        suggestions.push_back(std::move(suggestion));
    }

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
        bool duplicate = false;
        for (const SuggestedRegion& existing : suggestions) {
            if (PracticallyEqual(existing.region, suggestion.region)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) suggestions.push_back(std::move(suggestion));
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
