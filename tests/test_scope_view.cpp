#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "modules/module_registry.h"
#include "sidescopes/module.h"

namespace sidescopes {
namespace {

// The built-in scope registry, shared across the cases: it is immutable, so one
// instance serves every ScopeView under test.
const ScopeRegistry& registry()
{
    static const ScopeRegistry instance{builtinModules()};

    return instance;
}

// A scope whose requested letter is the host-reserved 'C', so the registry
// registers it letterless: the only way it can appear in the stack is by id.
constexpr char LetterlessId[] = "org.sidescopes.test.letterless";

bool trueInit()
{
    return true;
}

void noopDeinit()
{
}

uint32_t oneScope()
{
    return 1;
}

SsScopeInstance* nullCreate(const char*, const SsHost*)
{
    return nullptr;
}

const SsScopeDescriptor LetterlessDescriptor{
    LetterlessId, "Letterless", 'C', 0, 0, 0u, nullptr, 0u, 0.0f,
};

const SsScopeDescriptor* letterlessDescriptor(uint32_t index)
{
    return index == 0 ? &LetterlessDescriptor : nullptr;
}

const SsModuleEntry LetterlessModuleEntry{
    SS_ABI_MAJOR, SS_ABI_MINOR, trueInit, noopDeinit, oneScope, letterlessDescriptor, nullCreate,
};

// A registry whose one module scope is letterless, alongside the appended host
// color picker; a letterless scope can only ride the stack as an id token.
ScopeRegistry letterlessRegistry()
{
    ModuleRegistry modules;
    (void)modules.registerModule(LetterlessModuleEntry);

    return ScopeRegistry{modules};
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

TEST_CASE("The enabled ids cover the whole stack")
{
    ScopeView view{registry()};
    view.restoreStack("V");
    CHECK(view.enabledScopeIds() == std::vector<std::string>{VectorscopeScopeId});

    view.restoreStack("VH");
    CHECK(view.enabledScopeIds() == std::vector<std::string>{VectorscopeScopeId, HistogramScopeId});
}

TEST_CASE("The color picker asks nothing of the worker")
{
    // It reads the sampled cursor color, not worker output, so it
    // contributes no id to the enabled set.
    ScopeView view{registry()};
    view.restoreStack("C");
    CHECK(view.enabledScopeIds().empty());
}

TEST_CASE("The stack round-trips through preference letters")
{
    ScopeView view{registry()};
    view.restoreStack("VWRHC");
    CHECK(view.stackTokens() == "VWRHC");
    CHECK(view.stack().size() == 5);

    SECTION("unknown letters are ignored")
    {
        view.restoreStack("VxH");
        CHECK(view.stackTokens() == "VH");
    }

    SECTION("naming nothing valid falls back to the vectorscope")
    {
        view.restoreStack("zzz");
        CHECK(view.stackTokens() == "V");
        view.restoreStack("");
        CHECK(view.stackTokens() == "V");
    }
}

TEST_CASE("The stack reads scopes by bracketed id token")
{
    ScopeView view{registry()};
    view.restoreStack("[org.sidescopes.histogram][org.sidescopes.waveform]");
    CHECK(view.stack() == std::vector<std::string>{HistogramScopeId, WaveformScopeId});
    // These built-ins carry letters, so they persist back as bare letters.
    CHECK(view.stackTokens() == "HW");
}

TEST_CASE("A stack drops a letter the registry rejects but keeps a known id")
{
    // R11: letter validation runs through the registry, never a fixed string,
    // so an unknown letter falls out while a valid id token is kept.
    ScopeView view{registry()};
    view.restoreStack("Q[org.sidescopes.histogram]");
    CHECK(view.stack() == std::vector<std::string>{HistogramScopeId});
}

TEST_CASE("A letterless scope survives a save as an id token")
{
    // The only carrier for a letterless scope is its id: it must both restore
    // from and persist back to a bracketed token, or it is silently dropped.
    const ScopeRegistry letterless = letterlessRegistry();
    const HostScope* scope = letterless.byId(LetterlessId);
    REQUIRE(scope != nullptr);
    REQUIRE(scope->letter == 0);

    ScopeView view{letterless};
    view.restoreStack(std::string("[") + LetterlessId + "]");
    REQUIRE(view.stack() == std::vector<std::string>{LetterlessId});
    CHECK(view.stackTokens() == std::string("[") + LetterlessId + "]");
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

TEST_CASE("The layout orientation round-trips and starts automatic")
{
    ScopeView view{registry()};
    CHECK(view.orientation() == LayoutOrientation::Automatic);  // the historical split
    view.setOrientation(LayoutOrientation::Horizontal);
    CHECK(view.orientation() == LayoutOrientation::Horizontal);
    view.setOrientation(LayoutOrientation::Vertical);
    CHECK(view.orientation() == LayoutOrientation::Vertical);
}

TEST_CASE("Pane weights default to one and are tracked per scope")
{
    ScopeView view{registry()};
    // An untouched scope weighs 1, so equal weights reproduce the even split.
    CHECK(view.weight(VectorscopeScopeId) == 1.0f);
    view.setWeight(VectorscopeScopeId, 2.5f);
    view.setWeight(HistogramScopeId, 0.5f);
    CHECK(view.weight(VectorscopeScopeId) == 2.5f);
    CHECK(view.weight(HistogramScopeId) == 0.5f);
    // A scope never resized keeps the default.
    CHECK(view.weight(WaveformScopeId) == 1.0f);
}

TEST_CASE("Stack weights follow the visible order")
{
    ScopeView view{registry()};
    view.restoreStack("VWH");
    view.setWeight(VectorscopeScopeId, 3.0f);
    view.setWeight(HistogramScopeId, 0.5f);
    // The waveform is untouched, so it reports the default weight in position.
    CHECK(view.stackWeights() == std::vector<float>{3.0f, 1.0f, 0.5f});
}

TEST_CASE("Setting weights in bulk replaces every stored weight")
{
    ScopeView view{registry()};
    view.setWeight(VectorscopeScopeId, 4.0f);
    view.setWeights({{WaveformScopeId, 2.0}, {HistogramScopeId, 0.25}});
    // The prior vectorscope weight is gone (back to the default); the new ones
    // apply. This is how a loaded preset takes over the layout.
    CHECK(view.weight(VectorscopeScopeId) == 1.0f);
    CHECK(view.weight(WaveformScopeId) == 2.0f);
    CHECK(view.weight(HistogramScopeId) == 0.25f);
}

TEST_CASE("The weights snapshot carries only the stored entries")
{
    ScopeView view{registry()};
    view.setWeight(VectorscopeScopeId, 1.5f);
    view.setWeight(WaveformScopeId, 0.5f);
    const std::map<std::string, double> snapshot = view.weightsSnapshot();
    CHECK(snapshot.size() == 2);
    CHECK(snapshot.at(VectorscopeScopeId) == 1.5);
    CHECK(snapshot.at(WaveformScopeId) == 0.5);
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
