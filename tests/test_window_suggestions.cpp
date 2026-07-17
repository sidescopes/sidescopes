// Unit tests for the picker's window-suggestion logic (window_suggestions.cpp):
// which of a display's on-screen windows are offered, in what order, and as
// what display-relative rectangles. The auxiliary-chrome and occlusion rules
// are exercised here rather than through the live window server.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "app/window_suggestions.h"

namespace sidescopes {
namespace {

using Catch::Approx;

// A display filling a 1000x500 space at the desktop origin, so a point maps to
// a tenth of a percent across and a fifth of a percent down.
constexpr DisplayGeometry Display{0.0, 0.0, 1000.0, 500.0};

std::vector<std::string> labelsOf(const std::vector<SuggestedRegion>& suggestions)
{
    std::vector<std::string> labels;
    labels.reserve(suggestions.size());
    for (const SuggestedRegion& suggestion : suggestions) {
        labels.push_back(suggestion.label);
    }

    return labels;
}

}  // namespace

TEST_CASE("An empty desktop offers nothing")
{
    CHECK(buildWindowSuggestions({}, Display, 5).empty());
}

TEST_CASE("Suggestions come out frontmost first")
{
    // Three disjoint windows, so every one is fully visible; the order is the
    // window server's front-to-back order.
    const std::vector<DesktopWindow> windows{
        {0.0, 0.0, 200.0, 200.0, "A"},
        {400.0, 0.0, 200.0, 200.0, "B"},
        {0.0, 300.0, 200.0, 200.0, "C"},
    };

    CHECK(labelsOf(buildWindowSuggestions(windows, Display, 5)) == std::vector<std::string>{"A", "B", "C"});
}

TEST_CASE("A window buried under the union of the ones in front drops out")
{
    // Two front windows each hide one half of the back window; neither hides it
    // alone, but together they bury it. This is the owner-reported case: a
    // window fully behind the visible ones must not be offered. Testing
    // coverage against a single front window at a time - the flaw this
    // replaces - would leave the buried window a candidate.
    const std::vector<DesktopWindow> windows{
        {0.0, 0.0, 500.0, 500.0, "LeftHalf"},
        {500.0, 0.0, 500.0, 500.0, "RightHalf"},
        {0.0, 0.0, 1000.0, 500.0, "Buried"},
    };

    CHECK(labelsOf(buildWindowSuggestions(windows, Display, 5)) == std::vector<std::string>{"LeftHalf", "RightHalf"});
}

TEST_CASE("A window still meaningfully exposed behind another is kept")
{
    // The front window covers the left half of the back one, which keeps 50
    // percent of itself - well over the visibility threshold.
    const std::vector<DesktopWindow> windows{
        {0.0, 0.0, 500.0, 500.0, "Front"},
        {0.0, 0.0, 1000.0, 500.0, "Back"},
    };

    CHECK(labelsOf(buildWindowSuggestions(windows, Display, 5)) == std::vector<std::string>{"Front", "Back"});
}

TEST_CASE("A window all but hidden behind a smaller one in front drops out")
{
    // A different-app window in front covers 90 percent of the one behind it,
    // leaving it under the visibility threshold, so only the front one is
    // offered.
    const std::vector<DesktopWindow> windows{
        {0.0, 0.0, 900.0, 500.0, "Browser"},
        {0.0, 0.0, 1000.0, 500.0, "Editor"},
    };

    CHECK(labelsOf(buildWindowSuggestions(windows, Display, 5)) == std::vector<std::string>{"Browser"});
}

TEST_CASE("Auxiliary chrome is neither offered nor an occluder")
{
    // A panel of the same application, mostly inside the bigger main window, is
    // auxiliary chrome: it is not offered, and - the point of excluding it
    // before the occlusion math - it does not bury the window it sits on. Were
    // it counted as an occluder it would hide 90 percent of the main window and
    // drop it below the threshold; instead the main window stays, at its own
    // full extent rather than the panel's.
    const std::vector<DesktopWindow> windows{
        {0.0, 0.0, 900.0, 500.0, "Editor"},   // the panel, in front
        {0.0, 0.0, 1000.0, 500.0, "Editor"},  // the main window
    };

    const auto suggestions = buildWindowSuggestions(windows, Display, 5);
    REQUIRE(suggestions.size() == 1);
    CHECK(suggestions[0].label == "Editor");
    CHECK(suggestions[0].region.rightPercent == Approx(100.0));  // the main window, not the 90 percent panel
}

TEST_CASE("The suggestion count is capped at the limit")
{
    // Six disjoint windows, but the offer is limited to three; the three kept
    // are the frontmost.
    const std::vector<DesktopWindow> windows{
        {0.0, 0.0, 100.0, 100.0, "1"},   {200.0, 0.0, 100.0, 100.0, "2"}, {400.0, 0.0, 100.0, 100.0, "3"},
        {600.0, 0.0, 100.0, 100.0, "4"}, {800.0, 0.0, 100.0, 100.0, "5"}, {0.0, 300.0, 100.0, 100.0, "6"},
    };

    CHECK(labelsOf(buildWindowSuggestions(windows, Display, 3)) == std::vector<std::string>{"1", "2", "3"});
}

TEST_CASE("Window rectangles become display-relative percentages")
{
    // A display offset into the desktop; a window's percentages are measured
    // from the display's own origin.
    constexpr DisplayGeometry Offset{100.0, 50.0, 1000.0, 500.0};
    const std::vector<DesktopWindow> windows{{100.0, 50.0, 500.0, 250.0, "X"}};

    const auto suggestions = buildWindowSuggestions(windows, Offset, 5);
    REQUIRE(suggestions.size() == 1);
    CHECK(suggestions[0].region.leftPercent == Approx(0.0));
    CHECK(suggestions[0].region.topPercent == Approx(0.0));
    CHECK(suggestions[0].region.rightPercent == Approx(50.0));
    CHECK(suggestions[0].region.bottomPercent == Approx(50.0));
}

TEST_CASE("A window overspilling the display is clamped to it")
{
    // A window larger than the display, offset off its top-left: the edges that
    // run past the display clamp to its bounds.
    const std::vector<DesktopWindow> windows{{-200.0, -100.0, 2000.0, 1000.0, "Huge"}};

    const auto suggestions = buildWindowSuggestions(windows, Display, 5);
    REQUIRE(suggestions.size() == 1);
    CHECK(suggestions[0].region.leftPercent == Approx(0.0));
    CHECK(suggestions[0].region.topPercent == Approx(0.0));
    CHECK(suggestions[0].region.rightPercent == Approx(100.0));
    CHECK(suggestions[0].region.bottomPercent == Approx(100.0));
}

}  // namespace sidescopes
