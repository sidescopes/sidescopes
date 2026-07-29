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

TEST_CASE("The application opens on the first preset")
{
    // The state a first run lands in. It used to be "no preset", reachable
    // only by never having touched one, where the toolbar showed a dash and
    // every slot refused to load.
    Fixture fixture;

    CHECK(fixture.presets.activeSlot() == 1);
    // Nothing has been arranged, so the slot already holds what is on screen.
    CHECK_FALSE(fixture.presets.syncActiveSlot());
}

TEST_CASE("The default layout is what a capture of a fresh view produces")
{
    // The invariant auto-save depends on: the arrangement an unvisited slot
    // restores has to read back through the same capture that decides whether
    // anything changed, or a fresh install would write to its slot every frame
    // and never settle.
    const Fixture fixture;
    const LayoutPreset defaults = fixture.presets.defaultLayout();

    CHECK(defaults.stack == fixture.view.stack().tokens());
    CHECK(defaults.orientation == orientationToInt(LayoutOrientation::Automatic));
    CHECK(defaults.weights.size() == 1);
    CHECK(defaults.weights.at(VectorscopeScopeId) == DefaultPaneWeight);
}

TEST_CASE("Arranging the view writes into the slot it is on")
{
    // The whole model: there is no save, so a slot IS what is on screen while
    // you are on it.
    Fixture fixture;
    arrangeSomethingElse(fixture);

    CHECK(fixture.presets.syncActiveSlot());
    CHECK(fixture.presets.at(1).stack == fixture.view.stack().tokens());
    // ...and having written it, there is nothing left to write until the next
    // change, so the file is not rewritten every frame.
    CHECK_FALSE(fixture.presets.syncActiveSlot());
}

TEST_CASE("Visiting a slot that holds nothing does not fill it")
{
    // A slot nothing has been saved into restores the default, and writing
    // that default straight back would cost the list the one thing it knows:
    // which slots are the user's own.
    Fixture fixture;
    arrangeSomethingElse(fixture);
    REQUIRE(fixture.presets.syncActiveSlot());

    CHECK_FALSE(fixture.presets.load(6).status.empty());
    CHECK(fixture.presets.activeSlot() == 6);
    CHECK_FALSE(fixture.presets.syncActiveSlot());
    CHECK(fixture.presets.at(6).stack.empty());
}

TEST_CASE("Switching away keeps what was arranged behind you")
{
    // The reading of a load that auto-save makes true, and the reason nothing
    // has to be confirmed on the way out.
    Fixture fixture;
    arrangeSomethingElse(fixture);
    REQUIRE(fixture.presets.syncActiveSlot());
    const std::string arranged = fixture.view.stack().tokens();

    CHECK_FALSE(fixture.presets.load(4).status.empty());
    CHECK(fixture.view.stack().ids() == std::vector<std::string>{VectorscopeScopeId});

    CHECK_FALSE(fixture.presets.load(1).status.empty());
    CHECK(fixture.view.stack().tokens() == arranged);
}

TEST_CASE("A slot named before it is filled loads under its own name")
{
    Fixture fixture;
    const LayoutPresetOutcome renamed = fixture.presets.rename(3, "Skin tones");
    REQUIRE(renamed.preferencesSaveDue);

    const LayoutPresetOutcome outcome = fixture.presets.load(3);
    CHECK(outcome.status == "Skin tones loaded");
}

TEST_CASE("Copying into a slot leaves you where you are")
{
    // Shift+digit, the one way to make something that holds still: every slot
    // you are ON keeps changing under you, so a copy is only useful if it
    // lands somewhere you are not and you stay put.
    Fixture fixture;
    arrangeSomethingElse(fixture);
    REQUIRE(fixture.presets.syncActiveSlot());
    const std::string arranged = fixture.view.stack().tokens();

    const LayoutPresetOutcome copied = fixture.presets.copyInto(7);
    CHECK(copied.preferencesSaveDue);
    CHECK(fixture.presets.activeSlot() == 1);
    CHECK(fixture.presets.at(7).stack == arranged);

    // The copy is inert: arranging on where you still are does not follow it.
    fixture.view.stack().choose(ParadeScopeId, true);
    REQUIRE(fixture.presets.syncActiveSlot());
    CHECK(fixture.presets.at(7).stack == arranged);
    CHECK(fixture.presets.at(1).stack != arranged);
}

TEST_CASE("A copy keeps the name of the slot it lands in")
{
    // A name outlives the layout it was given to, so stamping a layout over a
    // slot replaces what it holds and not what it is called.
    Fixture fixture;
    const LayoutPresetOutcome renamed = fixture.presets.rename(5, "Skin tones");
    REQUIRE(renamed.preferencesSaveDue);
    arrangeSomethingElse(fixture);

    const LayoutPresetOutcome copied = fixture.presets.copyInto(5);
    CHECK(copied.status == "Copied to Skin tones");
    CHECK(presetDisplayName(5, fixture.presets.at(5)) == "Skin tones");
    CHECK_FALSE(fixture.presets.at(5).stack.empty());
}

}  // namespace sidescopes
