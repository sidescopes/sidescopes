#include <catch2/catch_test_macros.hpp>

#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/analysis_worker.h"
#include "modules/module_registry.h"

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

TEST_CASE("ScopeView starts on the vectorscope")
{
    ScopeView view{registry()};
    CHECK(view.shows(VectorscopeScopeId));
    CHECK_FALSE(view.shows(HistogramScopeId));
    CHECK(view.stack().size() == 1);
}

TEST_CASE("Toggling adds a scope and reports it newly visible")
{
    ScopeView view{registry()};
    CHECK(view.toggle(HistogramScopeId));
    CHECK(view.shows(HistogramScopeId));
    CHECK(view.stack().size() == 2);
    // Toggling it back off is not an activation.
    CHECK_FALSE(view.toggle(HistogramScopeId));
    CHECK_FALSE(view.shows(HistogramScopeId));
}

TEST_CASE("The last scope cannot be toggled away")
{
    ScopeView view{registry()};
    REQUIRE(view.stack().size() == 1);
    CHECK_FALSE(view.toggle(VectorscopeScopeId));
    CHECK(view.stack().size() == 1);
    CHECK(view.shows(VectorscopeScopeId));
}

TEST_CASE("Choosing solos a scope unless stacking")
{
    ScopeView view{registry()};
    view.toggle(WaveformScopeId);
    REQUIRE(view.stack().size() == 2);

    SECTION("solo replaces the stack")
    {
        CHECK(view.choose(HistogramScopeId, false));
        CHECK(view.stack().size() == 1);
        CHECK(view.shows(HistogramScopeId));
        CHECK_FALSE(view.shows(VectorscopeScopeId));
    }

    SECTION("soloing an already-shown scope is not an activation")
    {
        CHECK_FALSE(view.choose(WaveformScopeId, false));
        CHECK(view.stack().size() == 1);
        CHECK(view.shows(WaveformScopeId));
    }

    SECTION("stacking keeps the others")
    {
        CHECK(view.choose(HistogramScopeId, true));
        CHECK(view.stack().size() == 3);
    }
}

TEST_CASE("The enabled mask covers the whole stack")
{
    ScopeView view{registry()};
    view.restoreStack("V");
    CHECK(view.enabledMask() == ScopeVectorscope);

    view.restoreStack("VH");
    CHECK(view.enabledMask() == (ScopeVectorscope | ScopeHistogram));
}

TEST_CASE("The color picker asks nothing of the worker")
{
    // It reads the sampled cursor color, not worker output, so it
    // contributes no bit to the enabled mask.
    ScopeView view{registry()};
    view.restoreStack("C");
    CHECK(view.enabledMask() == 0u);
}

TEST_CASE("The stack round-trips through preference letters")
{
    ScopeView view{registry()};
    view.restoreStack("VWRHC");
    CHECK(view.stackLetters() == "VWRHC");
    CHECK(view.stack().size() == 5);

    SECTION("unknown letters are ignored")
    {
        view.restoreStack("VxH");
        CHECK(view.stackLetters() == "VH");
    }

    SECTION("naming nothing valid falls back to the vectorscope")
    {
        view.restoreStack("zzz");
        CHECK(view.stackLetters() == "V");
        view.restoreStack("");
        CHECK(view.stackLetters() == "V");
    }
}

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

TEST_CASE("The graticule toggle round-trips")
{
    ScopeView view{registry()};
    CHECK(view.graticule());  // shown by default
    view.setGraticule(false);
    CHECK_FALSE(view.graticule());
    view.setGraticule(true);
    CHECK(view.graticule());
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

TEST_CASE("Intensity and smoothing are tracked per trace")
{
    ScopeView view{registry()};
    view.setIntensity(VectorscopeScopeId, 40.0f);
    view.setIntensity(WaveformScopeId, 60.0f);
    CHECK(view.intensity(VectorscopeScopeId) == 40.0f);
    CHECK(view.intensity(WaveformScopeId) == 60.0f);

    view.setSmoothing(VectorscopeScopeId, 120.0f);
    view.setSmoothing(WaveformScopeId, 250.0f);
    CHECK(view.smoothing(VectorscopeScopeId) == 120.0f);
    CHECK(view.smoothing(WaveformScopeId) == 250.0f);

    // The parade shares the waveform's single trace control.
    CHECK(view.intensity(ParadeScopeId) == 60.0f);
    CHECK(view.smoothing(ParadeScopeId) == 250.0f);
    view.setIntensity(ParadeScopeId, 70.0f);
    CHECK(view.intensity(WaveformScopeId) == 70.0f);
}

}  // namespace sidescopes
