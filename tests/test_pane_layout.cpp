#include <catch2/catch_test_macros.hpp>
#include <map>
#include <string>
#include <vector>

#include "app/pane_layout.h"
#include "app/scope_registry.h"

namespace sidescopes {

TEST_CASE("The layout orientation round-trips and starts automatic")
{
    PaneLayout layout;
    CHECK(layout.orientation() == LayoutOrientation::Automatic);  // the historical split
    layout.setOrientation(LayoutOrientation::Horizontal);
    CHECK(layout.orientation() == LayoutOrientation::Horizontal);
    layout.setOrientation(LayoutOrientation::Vertical);
    CHECK(layout.orientation() == LayoutOrientation::Vertical);
}

TEST_CASE("Pane weights default to one and are tracked per scope")
{
    PaneLayout layout;
    // An untouched scope weighs 1, so equal weights reproduce the even split.
    CHECK(layout.weight(VectorscopeScopeId) == 1.0f);
    layout.setWeight(VectorscopeScopeId, 2.5f);
    layout.setWeight(HistogramScopeId, 0.5f);
    CHECK(layout.weight(VectorscopeScopeId) == 2.5f);
    CHECK(layout.weight(HistogramScopeId) == 0.5f);
    // A scope never resized keeps the default.
    CHECK(layout.weight(WaveformScopeId) == 1.0f);
}

TEST_CASE("Stack weights follow the order they are asked for")
{
    PaneLayout layout;
    layout.setWeight(VectorscopeScopeId, 3.0f);
    layout.setWeight(HistogramScopeId, 0.5f);
    // The waveform is untouched, so it reports the default weight in position.
    const std::vector<std::string> stack{VectorscopeScopeId, WaveformScopeId, HistogramScopeId};
    CHECK(layout.stackWeights(stack) == std::vector<float>{3.0f, 1.0f, 0.5f});
    // Nothing on screen is nothing to weigh.
    CHECK(layout.stackWeights({}).empty());
}

TEST_CASE("A weight outlives the scope leaving the stack")
{
    // Weights are keyed by id alone, so toggling a scope off and on again
    // restores the split the user dragged.
    PaneLayout layout;
    layout.setWeight(WaveformScopeId, 2.0f);
    CHECK(layout.stackWeights({VectorscopeScopeId}) == std::vector<float>{1.0f});
    CHECK(layout.stackWeights({VectorscopeScopeId, WaveformScopeId}) == std::vector<float>{1.0f, 2.0f});
}

TEST_CASE("Setting weights in bulk replaces every stored weight")
{
    PaneLayout layout;
    layout.setWeight(VectorscopeScopeId, 4.0f);
    layout.setWeights({{WaveformScopeId, 2.0}, {HistogramScopeId, 0.25}});
    // The prior vectorscope weight is gone (back to the default); the new ones
    // apply. This is how a loaded preset takes over the layout.
    CHECK(layout.weight(VectorscopeScopeId) == 1.0f);
    CHECK(layout.weight(WaveformScopeId) == 2.0f);
    CHECK(layout.weight(HistogramScopeId) == 0.25f);
}

TEST_CASE("The weights snapshot carries only the stored entries")
{
    PaneLayout layout;
    layout.setWeight(VectorscopeScopeId, 1.5f);
    layout.setWeight(WaveformScopeId, 0.5f);
    const std::map<std::string, double> snapshot = layout.weightsSnapshot();
    CHECK(snapshot.size() == 2);
    CHECK(snapshot.at(VectorscopeScopeId) == 1.5);
    CHECK(snapshot.at(WaveformScopeId) == 0.5);
}

}  // namespace sidescopes
