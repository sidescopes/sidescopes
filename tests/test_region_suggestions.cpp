#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/region_suggestions.h"

namespace sidescopes {

TEST_CASE("Suggestions list windows frontmost first") {
    std::vector<WindowRegion> windows{
        {RegionOfInterest{5.0, 5.0, 60.0, 90.0}, "Lightroom"},
        {RegionOfInterest{40.0, 10.0, 95.0, 80.0}, "Preview"},
    };

    const auto suggestions = BuildRegionSuggestions(windows);
    REQUIRE(suggestions.size() == 2);
    CHECK(suggestions[0].label == "Lightroom");
    CHECK(suggestions[1].label == "Preview");
    CHECK(suggestions[0].region.left_percent == 5.0);
}

TEST_CASE("Suggestions deduplicate practically identical windows") {
    std::vector<WindowRegion> windows{
        {RegionOfInterest{5.0, 5.0, 60.0, 90.0}, "Lightroom"},
        {RegionOfInterest{5.5, 4.6, 60.9, 89.2}, "Lightroom helper"},
    };

    const auto suggestions = BuildRegionSuggestions(windows);
    REQUIRE(suggestions.size() == 1);
    CHECK(suggestions[0].label == "Lightroom");
}

TEST_CASE("Suggestions survive an empty desktop") {
    CHECK(BuildRegionSuggestions({}).empty());
}

}  // namespace sidescopes
