#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "app/layout_presets.h"
#include "app/pane_layout.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/analysis_worker.h"
#include "modules/module_registry.h"

namespace sidescopes {
namespace {

// The real registry, the real view, the real settings: the default layout is
// built from what the modules declare, so a stub registry would prove nothing
// about the arrangement the application actually opens on.
struct Fixture
{
    ScopeRegistry registry{builtinModules()};
    ScopeView view{registry};
    AnalysisSettings analysis;
    LayoutPresetController presets{view, registry, analysis};
};

// Puts the view somewhere clearly not the default: more scopes, a forced
// split, an uneven divider.
void arrangeSomethingElse(Fixture& fixture)
{
    fixture.view.stack().choose(WaveformScopeId, true);
    fixture.view.stack().choose(HistogramScopeId, true);
    fixture.view.layout().setOrientation(LayoutOrientation::Horizontal);
    fixture.view.layout().setWeight(WaveformScopeId, 2.5f);
}

}  // namespace

TEST_CASE("The application opens on the first preset with nothing drifted")
{
    // The state a first run lands in. It used to be "no preset", reachable
    // only by never having touched one, where the toolbar showed a dash and
    // every slot refused to load.
    const Fixture fixture;

    CHECK(fixture.presets.activeSlot() == 1);
    CHECK_FALSE(fixture.presets.activeDirty());
}

TEST_CASE("The default layout is what a capture of a fresh view produces")
{
    // The invariant the star depends on: the arrangement an unsaved slot
    // restores must read back through the same capture the star compares
    // against, field for field, or a fresh install would open already starred.
    const Fixture fixture;
    const LayoutPreset defaults = fixture.presets.defaultLayout();

    CHECK(defaults.stack == fixture.view.stack().tokens());
    CHECK(defaults.orientation == orientationToInt(LayoutOrientation::Automatic));
    CHECK(defaults.weights.size() == 1);
    CHECK(defaults.weights.at(VectorscopeScopeId) == DefaultPaneWeight);
}

TEST_CASE("Loading a slot that holds nothing restores the default layout")
{
    // The dead end this replaces: clicking an unsaved slot reported an error
    // on the status strip and did nothing at all.
    Fixture fixture;
    arrangeSomethingElse(fixture);
    REQUIRE(fixture.view.stack().ids().size() == 3);

    const LayoutPresetOutcome outcome = fixture.presets.load(6);

    CHECK(fixture.view.stack().ids() == std::vector<std::string>{VectorscopeScopeId});
    CHECK(fixture.view.layout().orientation() == LayoutOrientation::Automatic);
    CHECK(fixture.view.layout().weight(WaveformScopeId) == DefaultPaneWeight);
    // It loaded, and says so: no slot reports that it cannot be used.
    CHECK(outcome.status == "Preset 6 loaded");
    CHECK(outcome.analysisDirty);
    CHECK(fixture.presets.activeSlot() == 6);
    // ...and the layout on screen is exactly what that slot restores, so the
    // toolbar carries no star the moment after a load.
    CHECK_FALSE(fixture.presets.activeDirty());
}

TEST_CASE("A slot named before it is filled loads under its own name")
{
    Fixture fixture;
    const LayoutPresetOutcome renamed = fixture.presets.rename(3, "Skin tones");
    REQUIRE(renamed.preferencesSaveDue);

    const LayoutPresetOutcome outcome = fixture.presets.load(3);
    CHECK(outcome.status == "Skin tones loaded");
}

TEST_CASE("A saved slot loads what it holds rather than the defaults")
{
    Fixture fixture;
    arrangeSomethingElse(fixture);
    const LayoutPresetOutcome saved = fixture.presets.save(2);
    CHECK(saved.status == "Preset 2 saved");
    CHECK(saved.preferencesSaveDue);
    CHECK_FALSE(fixture.presets.activeDirty());

    // Away to a slot holding nothing, then back: the arrangement returns.
    CHECK_FALSE(fixture.presets.load(7).status.empty());
    REQUIRE(fixture.view.stack().ids().size() == 1);

    CHECK_FALSE(fixture.presets.load(2).status.empty());
    CHECK(fixture.view.stack().ids().size() == 3);
    CHECK(fixture.view.layout().orientation() == LayoutOrientation::Horizontal);
    CHECK(fixture.view.layout().weight(WaveformScopeId) == 2.5f);
    CHECK_FALSE(fixture.presets.activeDirty());
}

TEST_CASE("Saving into another slot moves onto that slot")
{
    // The rule behind the row's save button, and the only part of it that is
    // not self-evident: saving into the slot you are on updates it, and saving
    // into any other takes the live layout there AND moves you to it. One rule
    // covers both, and it is the only one that leaves nothing drifted, since
    // the star measures the live layout against the ACTIVE slot.
    Fixture fixture;
    REQUIRE(fixture.presets.activeSlot() == 1);
    arrangeSomethingElse(fixture);
    REQUIRE(fixture.presets.activeDirty());

    CHECK_FALSE(fixture.presets.save(5).status.empty());
    CHECK(fixture.presets.activeSlot() == 5);
    CHECK_FALSE(fixture.presets.activeDirty());
    // The slot left behind was not written to on the way past.
    CHECK(fixture.presets.at(1).stack.empty());
    CHECK_FALSE(fixture.presets.at(5).stack.empty());

    // Saving again, now into the slot already active, updates it in place.
    fixture.view.stack().choose(ParadeScopeId, true);
    REQUIRE(fixture.presets.activeDirty());
    CHECK_FALSE(fixture.presets.save(5).status.empty());
    CHECK(fixture.presets.activeSlot() == 5);
    CHECK_FALSE(fixture.presets.activeDirty());
}

TEST_CASE("Changing the live layout stars the active slot")
{
    Fixture fixture;
    REQUIRE_FALSE(fixture.presets.activeDirty());

    arrangeSomethingElse(fixture);
    CHECK(fixture.presets.activeDirty());

    // Saving into the slot that is starred settles it again.
    CHECK_FALSE(fixture.presets.save(1).status.empty());
    CHECK_FALSE(fixture.presets.activeDirty());
}

}  // namespace sidescopes
