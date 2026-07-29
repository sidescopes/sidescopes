#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "modules/module_registry.h"
#include "support/scope_tokens.h"

namespace sidescopes {
namespace {

// The built-in scope registry, shared across the cases: it is immutable, so one
// instance serves every ScopeView under test.
const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{builtinModules()};

    return instance;
}

}  // namespace

TEST_CASE("The trace flash remembers which trace was adjusted")
{
    TraceFlash flash;
    CHECK_FALSE(flash.showing(VectorscopeScopeId, 0.0));

    flash.show(VectorscopeScopeId, 10.0);
    CHECK(flash.showing(VectorscopeScopeId, 9.0));
    CHECK_FALSE(flash.showing(WaveformScopeId, 9.0));
    CHECK_FALSE(flash.showing(VectorscopeScopeId, 10.0));
    CHECK_FALSE(flash.showing(VectorscopeScopeId, 11.0));

    // The newest gesture wins.
    flash.show(WaveformScopeId, 20.0);
    CHECK(flash.showing(WaveformScopeId, 15.0));
    CHECK_FALSE(flash.showing(VectorscopeScopeId, 15.0));
}

TEST_CASE("The graticule strength round-trips, snapped to a step")
{
    ScopeView view{registry()};
    CHECK(view.graticuleStrength() == DefaultGraticuleStrength);
    view.setGraticuleStrength(GraticuleStrengths.front());
    CHECK(view.graticuleStrength() == GraticuleStrengths.front());

    // The setter is the one gate on the value, so what reaches the ink is
    // always a step: a hand-edited preferences file cannot dim the graticule
    // past the floor, and nothing can switch it off.
    view.setGraticuleStrength(0.05f);
    CHECK(view.graticuleStrength() == GraticuleStrengths.front());
    view.setGraticuleStrength(0.0f);
    CHECK(view.graticuleStrength() == DefaultGraticuleStrength);
}

TEST_CASE("The magnify zoom round-trips and is stored verbatim")
{
    ScopeView view{registry()};
    CHECK(view.zoom() == 1);  // unmagnified by default
    view.setZoom(2);
    CHECK(view.zoom() == 2);
    view.setZoom(4);
    CHECK(view.zoom() == 4);
    // setZoom does not clamp: the valid 1/2/4 set is enforced at the
    // preferences boundary, not here, so an off-scale value is stored as is.
    view.setZoom(3);
    CHECK(view.zoom() == 3);
}

TEST_CASE("The view holds its three parts and hands them out")
{
    ScopeView view{registry()};
    view.stack().restore(testing::idTokens("VH"));
    view.layout().setWeight(HistogramScopeId, 2.0f);
    view.traces().setIntensity(WaveformScopeId, 40.0f);

    const ScopeView& reading = view;
    CHECK(reading.stack().ids() == std::vector<std::string>{VectorscopeScopeId, HistogramScopeId});
    CHECK(reading.layout().stackWeights(reading.stack().ids()) == std::vector<float>{1.0f, 2.0f});
    CHECK(reading.traces().intensity(ParadeScopeId) == 40.0f);
}

TEST_CASE("Reordering a scope carries the panes with it")
{
    // The drag is one gesture, so the menu order and the panes move together
    // rather than leaving a caller to remember the second half.
    ScopeView view{registry()};
    view.stack().restore(testing::idTokens("VWH"));
    REQUIRE(view.stack().tokens() == testing::idTokens("VWH"));

    // The rows are indexed in the MENU, which lists every scope, so a row's
    // index is not its pane's: the histogram is the third pane and a later row.
    const int histogram = static_cast<int>(view.order().rank(HistogramScopeId));
    REQUIRE(histogram > 2);
    CHECK(view.reorderScopes(histogram, 0));
    CHECK(view.order().ids().front() == HistogramScopeId);
    CHECK(view.stack().tokens() == testing::idTokens("HVW"));

    // A drop that changes nothing moves nothing, so nothing is saved for it.
    CHECK_FALSE(view.reorderScopes(0, 1));
    CHECK(view.stack().tokens() == testing::idTokens("HVW"));
}

TEST_CASE("Reordering a scope that is not on screen still holds its place")
{
    // The order covers every scope, on screen or not: the point of it is that
    // a scope switched on later lands where it was put, not at the end.
    ScopeView view{registry()};
    view.stack().restore(testing::idTokens("V"));
    const std::size_t histogram = view.order().rank(HistogramScopeId);
    REQUIRE(histogram > 0);

    REQUIRE(view.reorderScopes(static_cast<int>(histogram), 0));
    CHECK(view.stack().tokens() == testing::idTokens("V"));  // the panes are unchanged: it is not shown

    view.stack().toggle(HistogramScopeId);
    CHECK(view.stack().tokens() == testing::idTokens("HV"));
}

TEST_CASE("Two views over one registry keep their own state")
{
    // Nothing in the view is shared or global, so a second one is a second
    // independent set of scopes on the same host.
    ScopeView first{registry()};
    ScopeView second{registry()};
    first.stack().restore(testing::idTokens("VW"));
    second.stack().restore(testing::idTokens("H"));
    second.setZoom(4);

    CHECK(first.stack().tokens() == testing::idTokens("VW"));
    CHECK(second.stack().tokens() == testing::idTokens("H"));
    CHECK(first.zoom() == 1);
}

}  // namespace sidescopes
