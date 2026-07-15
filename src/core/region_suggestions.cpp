#include "core/region_suggestions.h"

#include <cmath>

namespace sidescopes {
namespace {

// Two regions within this many percent on every edge count as the same
// suggestion.
constexpr double DuplicateTolerancePercent = 2.0;

bool practicallyEqual(const RegionOfInterest& a, const RegionOfInterest& b)
{
    return std::abs(a.leftPercent - b.leftPercent) <= DuplicateTolerancePercent &&
           std::abs(a.topPercent - b.topPercent) <= DuplicateTolerancePercent &&
           std::abs(a.rightPercent - b.rightPercent) <= DuplicateTolerancePercent &&
           std::abs(a.bottomPercent - b.bottomPercent) <= DuplicateTolerancePercent;
}

}  // namespace

std::vector<SuggestedRegion> buildRegionSuggestions(const std::vector<WindowRegion>& windows)
{
    std::vector<SuggestedRegion> suggestions;
    for (const WindowRegion& window : windows) {
        bool duplicate = false;
        for (const SuggestedRegion& existing : suggestions) {
            if (practicallyEqual(existing.region, window.region)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        SuggestedRegion suggestion;
        suggestion.region = window.region;
        suggestion.label = window.application;
        suggestions.push_back(std::move(suggestion));
    }
    return suggestions;
}

std::vector<SuggestedRegion> buildFaceSuggestions(const std::vector<IntRect>& faces, int frameWidth, int frameHeight)
{
    // The inward shrink per side: enough to shed hair and background at
    // the box edges without losing the cheeks.
    constexpr double InsetFraction = 0.1;

    std::vector<SuggestedRegion> suggestions;
    if (frameWidth <= 0 || frameHeight <= 0) {
        return suggestions;
    }
    for (const IntRect& face : faces) {
        const double insetX = face.width * InsetFraction;
        const double insetY = face.height * InsetFraction;
        SuggestedRegion suggestion;
        suggestion.region.leftPercent = (face.x + insetX) * 100.0 / frameWidth;
        suggestion.region.topPercent = (face.y + insetY) * 100.0 / frameHeight;
        suggestion.region.rightPercent = (face.x + face.width - insetX) * 100.0 / frameWidth;
        suggestion.region.bottomPercent = (face.y + face.height - insetY) * 100.0 / frameHeight;
        suggestion.label = "Face";
        bool duplicate = false;
        for (const SuggestedRegion& existing : suggestions) {
            if (practicallyEqual(existing.region, suggestion.region)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            suggestions.push_back(std::move(suggestion));
        }
    }
    return suggestions;
}

}  // namespace sidescopes
