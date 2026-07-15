#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/region_suggestions.h"

namespace sidescopes {

TEST_CASE("Suggestions list windows frontmost first")
{
    std::vector<WindowRegion> windows{
        {RegionOfInterest{5.0, 5.0, 60.0, 90.0}, "Lightroom"},
        {RegionOfInterest{40.0, 10.0, 95.0, 80.0}, "Preview"},
    };

    const auto suggestions = buildRegionSuggestions(windows);
    REQUIRE(suggestions.size() == 2);
    CHECK(suggestions[0].label == "Lightroom");
    CHECK(suggestions[1].label == "Preview");
    CHECK(suggestions[0].region.leftPercent == 5.0);
}

TEST_CASE("Suggestions deduplicate practically identical windows")
{
    std::vector<WindowRegion> windows{
        {RegionOfInterest{5.0, 5.0, 60.0, 90.0}, "Lightroom"},
        {RegionOfInterest{5.5, 4.6, 60.9, 89.2}, "Lightroom helper"},
    };

    const auto suggestions = buildRegionSuggestions(windows);
    REQUIRE(suggestions.size() == 1);
    CHECK(suggestions[0].label == "Lightroom");
}

TEST_CASE("Suggestions survive an empty desktop")
{
    CHECK(buildRegionSuggestions({}).empty());
}

TEST_CASE("Face suggestions shrink the detector box inward")
{
    // A 200x200 face at 100,100 in a 1000x500 frame, 10 percent inset per
    // side: skin, not hair and background.
    const auto suggestions = buildFaceSuggestions({IntRect{100, 100, 200, 200}}, 1000, 500);
    REQUIRE(suggestions.size() == 1);
    CHECK(suggestions[0].label == "Face");
    CHECK(suggestions[0].region.leftPercent == 12.0);
    CHECK(suggestions[0].region.rightPercent == 28.0);
    CHECK(suggestions[0].region.topPercent == 24.0);
    CHECK(suggestions[0].region.bottomPercent == 56.0);
}

TEST_CASE("Face suggestions deduplicate overlapping detections")
{
    const auto suggestions = buildFaceSuggestions({IntRect{100, 100, 200, 200}, IntRect{102, 99, 201, 202}}, 1000, 500);
    CHECK(suggestions.size() == 1);
}

}  // namespace sidescopes
