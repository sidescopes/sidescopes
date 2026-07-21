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

TEST_CASE("Face suggestions reject a degenerate frame")
{
    // Percentages need positive frame dimensions; a zero or negative extent
    // yields nothing rather than dividing by it.
    CHECK(buildFaceSuggestions({IntRect{10, 10, 40, 40}}, 0, 100).empty());
    CHECK(buildFaceSuggestions({IntRect{10, 10, 40, 40}}, 100, 0).empty());
    CHECK(buildFaceSuggestions({IntRect{10, 10, 40, 40}}, -100, 100).empty());
}

TEST_CASE("Face suggestions keep two well-separated faces")
{
    const auto suggestions = buildFaceSuggestions({IntRect{0, 0, 100, 100}, IntRect{800, 300, 120, 120}}, 1000, 500);
    CHECK(suggestions.size() == 2);
}

TEST_CASE("Face offers carry the display and the frame they were measured on")
{
    const auto offers = buildFaceOffers({IntRect{100, 100, 200, 200}}, 7, 1000, 500);
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].displayId == 7);
    CHECK(offers[0].frameWidth == 1000);
    CHECK(offers[0].frameHeight == 500);
    // The raw detector box is kept unshrunk for the pin to anchor on.
    CHECK(offers[0].box.x == 100);
    CHECK(offers[0].box.width == 200);
}

TEST_CASE("Face offers agree with the overlay's suggestion regions")
{
    // An offer's region and its display's suggestion for the same box are the
    // same shrunk rectangle, so a confirmed offer matches its box regardless
    // of which display it came from.
    const std::vector<IntRect> faces{IntRect{100, 100, 200, 200}};
    const auto offers = buildFaceOffers(faces, 1, 1000, 500);
    const auto suggestions = buildFaceSuggestions(faces, 1000, 500);
    REQUIRE(offers.size() == 1);
    REQUIRE(suggestions.size() == 1);
    CHECK(offers[0].region.leftPercent == suggestions[0].region.leftPercent);
    CHECK(offers[0].region.rightPercent == suggestions[0].region.rightPercent);
    CHECK(offers[0].region.topPercent == suggestions[0].region.topPercent);
    CHECK(offers[0].region.bottomPercent == suggestions[0].region.bottomPercent);
}

TEST_CASE("Face offers keep per-display frame dimensions distinct")
{
    // The same box on two displays of different resolutions must not collapse
    // to one frame: each offer maps back through its own dimensions.
    const std::vector<IntRect> box{IntRect{50, 50, 100, 100}};
    const auto small = buildFaceOffers(box, 1, 400, 300);
    const auto large = buildFaceOffers(box, 2, 3840, 2160);
    REQUIRE(small.size() == 1);
    REQUIRE(large.size() == 1);
    CHECK(small[0].frameWidth == 400);
    CHECK(large[0].frameWidth == 3840);
    // Same pixel box, different frames, so different offered percentages.
    CHECK(small[0].region.leftPercent != large[0].region.leftPercent);
}

TEST_CASE("Face offers reject a degenerate frame")
{
    CHECK(buildFaceOffers({IntRect{10, 10, 40, 40}}, 1, 0, 100).empty());
    CHECK(buildFaceOffers({IntRect{10, 10, 40, 40}}, 1, 100, 0).empty());
}

TEST_CASE("Face offers are empty for no detections")
{
    CHECK(buildFaceOffers({}, 1, 1000, 500).empty());
}

}  // namespace sidescopes
