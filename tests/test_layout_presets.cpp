#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "app/layout_presets.h"
#include "app/pane_layout.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/stack_tokens.h"
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

TEST_CASE("The order the panes sit in belongs to the slot")
{
    // It used to be one global setting, so loading a preset restored WHICH
    // scopes were shown but not how they were laid out - half a restore.
    Fixture fixture;
    fixture.view.stack().choose(WaveformScopeId, true);
    REQUIRE(fixture.view.reorderScopes(1, 0));
    const std::vector<std::string> arranged = fixture.view.order().ids();
    REQUIRE(fixture.presets.syncActiveSlot());

    // Away to another slot, which takes the order the modules register in...
    CHECK_FALSE(fixture.presets.load(3).status.empty());
    CHECK(fixture.view.order().ids() != arranged);

    // ...and back, which brings the arrangement with it.
    CHECK_FALSE(fixture.presets.load(1).status.empty());
    CHECK(fixture.view.order().ids() == arranged);
}

TEST_CASE("Moving a scope that is not shown is kept like any other")
{
    // THE CASE THAT WENT MISSING. Dragging a SHOWN scope also re-seats the
    // panes, so the stack's own tokens change and the move was written back as
    // a side effect of that. Move one that is NOT shown and the order is the
    // only thing that changed - so a comparison blind to the order dropped it
    // silently, and the menu forgot the move the moment the popup closed.
    Fixture fixture;
    const std::vector<std::string> registered = fixture.view.order().ids();
    REQUIRE(registered.size() > 2);
    const std::string& last = registered.back();
    REQUIRE_FALSE(fixture.view.stack().shows(last));
    const std::string shownStack = fixture.view.stack().tokens();

    REQUIRE(fixture.view.reorderScopes(static_cast<int>(registered.size()) - 1, 0));
    REQUIRE(fixture.view.order().ids().front() == last);
    // Nothing about which scopes are on screen moved.
    REQUIRE(fixture.view.stack().tokens() == shownStack);

    CHECK(fixture.presets.syncActiveSlot());
    CHECK(fixture.presets.at(1).order == fixture.view.order().tokens());
}

TEST_CASE("Switching slots carries the whole order, shown or not")
{
    // A scope's place is a property of the arrangement rather than of being on
    // screen, so a slot restores where every scope sits and not only where the
    // shown ones do.
    Fixture fixture;
    const std::size_t count = fixture.view.order().ids().size();
    REQUIRE(fixture.view.reorderScopes(static_cast<int>(count) - 1, 0));
    const std::vector<std::string> arranged = fixture.view.order().ids();
    REQUIRE(fixture.presets.syncActiveSlot());

    CHECK_FALSE(fixture.presets.load(4).status.empty());
    REQUIRE(fixture.view.order().ids() != arranged);

    CHECK_FALSE(fixture.presets.load(1).status.empty());
    CHECK(fixture.view.order().ids() == arranged);
    CHECK(fixture.view.order().ids().size() == count);
}

TEST_CASE("The panes and the menu are seated by one order")
{
    // They were deliberately unified: a scope brought back returns to the
    // place it was left, which only holds if the panes read the same order the
    // menu lists.
    Fixture fixture;
    fixture.view.stack().choose(WaveformScopeId, true);
    fixture.view.stack().choose(HistogramScopeId, true);
    REQUIRE(fixture.view.stack().ids().size() == 3);

    // Drag the histogram to the front of the MENU; the PANES follow.
    const std::vector<std::string>& order = fixture.view.order().ids();
    const auto at = std::find(order.begin(), order.end(), std::string{HistogramScopeId});
    REQUIRE(at != order.end());
    REQUIRE(fixture.view.reorderScopes(static_cast<int>(std::distance(order.begin(), at)), 0));

    CHECK(fixture.view.order().ids().front() == HistogramScopeId);
    CHECK(fixture.view.stack().ids().front() == HistogramScopeId);
}

TEST_CASE("A slot resumed from the file opens in its own order")
{
    // The order is restored from the slot being resumed rather than from a
    // global of its own, or the first frame would write the registry's order
    // over what the user had dragged.
    Fixture fixture;
    std::array<LayoutPreset, LayoutPresetSlots> stored;
    stored[1].stack = fixture.presets.defaultLayout().stack;
    stored[1].order = formatStackTokens(fixture.registry, {std::string{WaveformScopeId}});
    fixture.presets.restore(stored, 2);

    CHECK(fixture.view.order().ids().front() == WaveformScopeId);
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
