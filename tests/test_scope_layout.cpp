#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <numeric>
#include <vector>

#include "app/scope_layout.h"
#include "app/scope_view.h"

namespace sidescopes {
namespace {

using Catch::Approx;

// Sum of a length vector, for the invariant that panes fill the axis exactly.
float total(const std::vector<float>& lengths)
{
    return std::accumulate(lengths.begin(), lengths.end(), 0.0f);
}

}  // namespace

TEST_CASE("Automatic stacks a wide-scope stack in a landscape window")
{
    // The reported failure: three wide traces in a 1200x900 window went side
    // by side into 400x900 slivers; stacking gives each a full-width band.
    const std::vector<float> weights{1.0f, 1.0f, 1.0f};
    const std::vector<float> aspects{3.0f, 3.0f, 2.0f};
    CHECK(resolveSplitDirection(LayoutOrientation::Automatic, 1200.0f, 900.0f, weights, aspects, 6.0f) ==
          SplitDirection::Stacked);
}

TEST_CASE("Automatic lays squares side by side in a wide window")
{
    // Two square scopes in a 1600x500 strip read far better as 800x500
    // neighbors than as 1600x250 ribbons.
    const std::vector<float> weights{1.0f, 1.0f};
    const std::vector<float> aspects{1.0f, 1.0f};
    CHECK(resolveSplitDirection(LayoutOrientation::Automatic, 1600.0f, 500.0f, weights, aspects, 6.0f) ==
          SplitDirection::SideBySide);
}

TEST_CASE("Automatic ties and degenerate metadata stack")
{
    // A single pane scores identically both ways, and a weight list that
    // disagrees with the aspect list must not steer the layout.
    CHECK(resolveSplitDirection(LayoutOrientation::Automatic, 800.0f, 400.0f, {1.0f}, {3.0f}, 6.0f) ==
          SplitDirection::Stacked);
    CHECK(resolveSplitDirection(LayoutOrientation::Automatic, 800.0f, 400.0f, {1.0f, 1.0f}, {3.0f}, 6.0f) ==
          SplitDirection::Stacked);
}

TEST_CASE("Explicit orientation ignores area and aspects")
{
    // Vertical stacks and Horizontal lays side by side whatever the area
    // shape or the scopes' preferences.
    CHECK(resolveSplitDirection(LayoutOrientation::Vertical, 800.0f, 400.0f, {1.0f, 1.0f}, {1.0f, 1.0f}, 6.0f) ==
          SplitDirection::Stacked);
    CHECK(resolveSplitDirection(LayoutOrientation::Horizontal, 400.0f, 800.0f, {1.0f, 1.0f}, {3.0f, 3.0f}, 6.0f) ==
          SplitDirection::SideBySide);
}

TEST_CASE("Preferred aspects know the built-in scopes")
{
    // The wide traces want width, the vectorscope is square, and unknown
    // module ids fall back gently wide.
    CHECK(preferredScopeAspect(WaveformScopeId) == Approx(3.0f));
    CHECK(preferredScopeAspect(ParadeScopeId) == Approx(3.0f));
    CHECK(preferredScopeAspect(HistogramScopeId) == Approx(2.0f));
    CHECK(preferredScopeAspect(VectorscopeScopeId) == Approx(1.0f));
    CHECK(preferredScopeAspect("org.example.custom") == Approx(2.0f));
}

TEST_CASE("Equal weights split the axis evenly")
{
    const std::vector<float> lengths = paneLengths({1.0f, 1.0f, 1.0f}, 300.0f, 80.0f);
    REQUIRE(lengths.size() == 3);
    CHECK(lengths[0] == Approx(100.0f));
    CHECK(lengths[1] == Approx(100.0f));
    CHECK(lengths[2] == Approx(100.0f));
    CHECK(total(lengths) == Approx(300.0f));
}

TEST_CASE("Weights divide the axis in proportion")
{
    // A big vectorscope beside a narrow histogram: weight 3 to 1 over 400 units
    // with no min pressure gives 300 and 100.
    const std::vector<float> lengths = paneLengths({3.0f, 1.0f}, 400.0f, 40.0f);
    REQUIRE(lengths.size() == 2);
    CHECK(lengths[0] == Approx(300.0f));
    CHECK(lengths[1] == Approx(100.0f));
}

TEST_CASE("A pane never shrinks below the minimum while room remains")
{
    // Weights 100:1:1 would give a starved pair, so both clamp to the 80 floor
    // and the dominant pane takes the rest; the axis still fills exactly.
    const std::vector<float> lengths = paneLengths({100.0f, 1.0f, 1.0f}, 300.0f, 80.0f);
    REQUIRE(lengths.size() == 3);
    CHECK(lengths[0] == Approx(140.0f));
    CHECK(lengths[1] == Approx(80.0f));
    CHECK(lengths[2] == Approx(80.0f));
    CHECK(total(lengths) == Approx(300.0f));
}

TEST_CASE("Too little room falls back to an even split below the minimum")
{
    // Two panes with an 80 floor need 160; a 100-unit axis cannot honor it, so
    // they split evenly at 50 rather than overflow.
    const std::vector<float> lengths = paneLengths({3.0f, 1.0f}, 100.0f, 80.0f);
    REQUIRE(lengths.size() == 2);
    CHECK(lengths[0] == Approx(50.0f));
    CHECK(lengths[1] == Approx(50.0f));
}

TEST_CASE("Zero and negative weights fall back to an even share")
{
    // A degenerate all-zero weight set cannot drive proportions, so the panes
    // share the axis evenly instead of dividing by zero.
    const std::vector<float> lengths = paneLengths({0.0f, 0.0f}, 200.0f, 40.0f);
    REQUIRE(lengths.size() == 2);
    CHECK(lengths[0] == Approx(100.0f));
    CHECK(lengths[1] == Approx(100.0f));
}

TEST_CASE("An empty stack yields no lengths")
{
    CHECK(paneLengths({}, 300.0f, 80.0f).empty());
}

TEST_CASE("A single pane takes the whole axis")
{
    const std::vector<float> lengths = paneLengths({1.0f}, 250.0f, 80.0f);
    REQUIRE(lengths.size() == 1);
    CHECK(lengths[0] == Approx(250.0f));
}

TEST_CASE("Dragging a divider moves weight between two neighbors")
{
    // Two equal panes of 100px each; dragging 20px right makes the first 60% of
    // the pair, so its weight rises to 1.2 and the second falls to 0.8 - their
    // sum unchanged, so the other panes are untouched.
    const auto [first, second] = dragDividerWeights(1.0f, 1.0f, 100.0f, 100.0f, 20.0f, 80.0f);
    CHECK(first == Approx(1.2f));
    CHECK(second == Approx(0.8f));
    CHECK(first + second == Approx(2.0f));
}

TEST_CASE("A divider drag stops at the minimum pane size")
{
    // A 60px push would leave the second pane at 40px, under the 80 floor, so
    // the first clamps to 120 (leaving exactly 80): weights 1.2 and 0.8.
    const auto [first, second] = dragDividerWeights(1.0f, 1.0f, 100.0f, 100.0f, 60.0f, 80.0f);
    CHECK(first == Approx(1.2f));
    CHECK(second == Approx(0.8f));
}

TEST_CASE("A divider drag preserves uneven combined weight")
{
    // Neighbors weighing 3 and 1 (lengths 300 and 100); nudging 50px right keeps
    // their combined weight of 4, re-split by the new 350:50 length ratio.
    const auto [first, second] = dragDividerWeights(3.0f, 1.0f, 300.0f, 100.0f, 50.0f, 40.0f);
    CHECK(first == Approx(3.5f));
    CHECK(second == Approx(0.5f));
    CHECK(first + second == Approx(4.0f));
}

TEST_CASE("A divider drag with no room to honor two minimums is a no-op")
{
    // Combined 120px cannot hold two 80px panes, so the split is left as it is.
    const auto [first, second] = dragDividerWeights(1.0f, 1.0f, 60.0f, 60.0f, 10.0f, 80.0f);
    CHECK(first == Approx(1.0f));
    CHECK(second == Approx(1.0f));
}

TEST_CASE("Orientation round-trips through its persisted integer")
{
    CHECK(orientationToInt(LayoutOrientation::Automatic) == 0);
    CHECK(orientationToInt(LayoutOrientation::Vertical) == 1);
    CHECK(orientationToInt(LayoutOrientation::Horizontal) == 2);
    CHECK(orientationFromInt(0) == LayoutOrientation::Automatic);
    CHECK(orientationFromInt(1) == LayoutOrientation::Vertical);
    CHECK(orientationFromInt(2) == LayoutOrientation::Horizontal);
    // An out-of-range integer degrades to Automatic rather than a bad cast.
    CHECK(orientationFromInt(9) == LayoutOrientation::Automatic);
    CHECK(orientationFromInt(-1) == LayoutOrientation::Automatic);
}

}  // namespace sidescopes
