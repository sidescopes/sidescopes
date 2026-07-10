#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/region_suggestions.h"

namespace sidescopes {

TEST_CASE("Suggestions convert detector candidates to percent") {
    std::vector<RegionCandidate> candidates{{IntRect{100, 50, 300, 200}, 0.9f}};

    const auto suggestions = BuildRegionSuggestions(candidates, 1000, 500, {});
    REQUIRE(suggestions.size() == 1);
    CHECK(suggestions[0].label == "Area");
    CHECK(suggestions[0].region.left_percent == 10.0);
    CHECK(suggestions[0].region.top_percent == 10.0);
    CHECK(suggestions[0].region.right_percent == 40.0);
    CHECK(suggestions[0].region.bottom_percent == 50.0);
}

TEST_CASE("Suggestions drop low-confidence candidates") {
    std::vector<RegionCandidate> candidates{{IntRect{0, 0, 100, 100}, 0.2f}};
    CHECK(BuildRegionSuggestions(candidates, 1000, 500, {}).empty());
}

TEST_CASE("Suggestions list photos before windows") {
    std::vector<RegionCandidate> candidates{{IntRect{100, 50, 300, 200}, 0.9f}};
    std::vector<WindowRegion> windows{{RegionOfInterest{5.0, 5.0, 60.0, 90.0}, "Lightroom"}};

    const auto suggestions = BuildRegionSuggestions(candidates, 1000, 500, windows);
    REQUIRE(suggestions.size() == 2);
    CHECK(suggestions[0].label == "Area");
    CHECK(suggestions[1].label == "Lightroom");
}

TEST_CASE("Suggestions deduplicate a canvas that fills its window") {
    // Detector found 10..40 x 10..50 percent; the window is within the
    // duplicate tolerance of the same rectangle.
    std::vector<RegionCandidate> candidates{{IntRect{100, 50, 300, 200}, 0.9f}};
    std::vector<WindowRegion> windows{{RegionOfInterest{10.5, 9.0, 40.9, 50.4}, "Preview"}};

    const auto suggestions = BuildRegionSuggestions(candidates, 1000, 500, windows);
    REQUIRE(suggestions.size() == 1);
    CHECK(suggestions[0].label == "Area");
}

TEST_CASE("Suggestions survive a missing frame") {
    std::vector<RegionCandidate> candidates{{IntRect{0, 0, 100, 100}, 0.9f}};
    CHECK(BuildRegionSuggestions(candidates, 0, 0, {}).empty());
}

}  // namespace sidescopes
