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
    // Nothing has been arranged, so there is nothing to save.
    CHECK_FALSE(fixture.presets.activeDirty());
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
    CHECK(defaults.weights.size() == 2);
    CHECK(defaults.weights.at(VectorscopeScopeId) == DefaultPaneWeight);
    CHECK(defaults.weights.at(WaveformScopeId) == DefaultPaneWeight);
}

TEST_CASE("Arranging the view leaves the slot alone until it is saved")
{
    // THE PROPERTY THE WHOLE MODEL TURNS ON. A slot that followed the screen
    // by itself was pleasant until a mistyped scope letter rewrote a saved
    // arrangement. Nothing reaches a slot without being asked.
    Fixture fixture;
    arrangeSomethingElse(fixture);

    CHECK(fixture.presets.activeDirty());
    CHECK(fixture.presets.at(1).stack.empty());

    CHECK_FALSE(fixture.presets.saveInto(fixture.presets.activeSlot()).status.empty());
    CHECK(fixture.presets.at(1).stack == fixture.view.stack().tokens());
    CHECK_FALSE(fixture.presets.activeDirty());
}

TEST_CASE("Being unsaved survives a restart with the layout intact")
{
    // THE OTHER HALF, and what an undo history could never offer: a history
    // does not survive the process, so a wrong change followed by quitting
    // loses the layout for good. The WORKING state persists beside the slots,
    // so quitting while unsaved costs nothing and asks nothing.
    Fixture before;
    arrangeSomethingElse(before);
    REQUIRE(before.presets.activeDirty());
    const std::string arranged = before.view.stack().tokens();
    const std::string arrangedOrder = before.view.order().tokens();
    const std::array<LayoutPreset, LayoutPresetSlots> slots = before.presets.all();

    // A second session restores the slots and, separately, the working layout.
    Fixture after;
    after.presets.restore(slots, before.presets.activeSlot());
    after.view.order().restore(arrangedOrder);
    after.view.stack().restore(arranged);
    after.view.layout().setOrientation(before.view.layout().orientation());
    after.view.layout().setWeights(before.view.layout().weightsSnapshot());

    CHECK(after.view.stack().tokens() == arranged);
    // ...and it is still unsaved, worked out by comparing the two rather than
    // by any flag that was written down and could disagree with them.
    CHECK(after.presets.activeDirty());
}

TEST_CASE("Visiting a slot that holds nothing leaves it empty")
{
    // A slot nothing has been saved into restores the default. Opening it is
    // not a change, so nothing about it reads as needing saving, and the list
    // keeps the one thing it knows: which slots are the user's own.
    Fixture fixture;
    arrangeSomethingElse(fixture);
    REQUIRE_FALSE(fixture.presets.saveInto(fixture.presets.activeSlot()).status.empty());

    CHECK_FALSE(fixture.presets.load(6).status.empty());
    CHECK(fixture.presets.activeSlot() == 6);
    CHECK_FALSE(fixture.presets.activeDirty());
    CHECK(fixture.presets.at(6).stack.empty());

    // Saving makes it real.
    fixture.view.stack().choose(HistogramScopeId, true);
    CHECK(fixture.presets.activeDirty());
    CHECK_FALSE(fixture.presets.saveInto(fixture.presets.activeSlot()).status.empty());
    CHECK_FALSE(fixture.presets.at(6).stack.empty());
}

TEST_CASE("Loading another slot discards an unsaved layout")
{
    // The user asked to go elsewhere, so they go elsewhere. Nothing prompts:
    // what was unsaved was never in a slot to begin with, and a modal in the
    // way of a keystroke is worse than the case it guards.
    Fixture fixture;
    arrangeSomethingElse(fixture);
    REQUIRE(fixture.presets.activeDirty());

    CHECK_FALSE(fixture.presets.load(3).status.empty());
    CHECK(fixture.view.stack().ids() == std::vector<std::string>{VectorscopeScopeId, WaveformScopeId});
    CHECK_FALSE(fixture.presets.activeDirty());
}

TEST_CASE("A slot named before it is filled loads under its own name")
{
    Fixture fixture;
    const LayoutPresetOutcome renamed = fixture.presets.rename(3, "Skin tones");
    REQUIRE(renamed.preferencesSaveDue);

    const LayoutPresetOutcome outcome = fixture.presets.load(3);
    CHECK(outcome.analysisDirty);
    CHECK(outcome.preferencesSaveDue);
    CHECK(outcome.status == "Loaded \"Skin tones\"");
}

TEST_CASE("The order the panes sit in belongs to the slot")
{
    // It used to be one global setting, so loading a preset restored WHICH
    // scopes were shown but not how they were laid out - half a restore.
    Fixture fixture;
    REQUIRE(fixture.view.reorderScopes(1, 0));
    const std::vector<std::string> arranged = fixture.view.order().ids();
    REQUIRE_FALSE(fixture.presets.saveInto(fixture.presets.activeSlot()).status.empty());

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

    CHECK_FALSE(fixture.presets.saveInto(fixture.presets.activeSlot()).status.empty());
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
    REQUIRE_FALSE(fixture.presets.saveInto(fixture.presets.activeSlot()).status.empty());

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

TEST_CASE("Restoring slots does not disturb the working order")
{
    // The order the panes sit in is part of the working state as well as part
    // of each slot, and the two are MEANT to differ - that is what being
    // unsaved is. Restoring the slots must therefore not reach into the view:
    // doing so would discard exactly what the working state exists to keep.
    Fixture fixture;
    const std::size_t count = fixture.view.order().ids().size();
    REQUIRE(fixture.view.reorderScopes(static_cast<int>(count) - 1, 0));
    const std::vector<std::string> working = fixture.view.order().ids();

    std::array<LayoutPreset, LayoutPresetSlots> stored;
    stored[1].stack = fixture.presets.defaultLayout().stack;
    stored[1].order = formatStackTokens(fixture.registry, {std::string{WaveformScopeId}});
    fixture.presets.restore(stored, 2);

    CHECK(fixture.view.order().ids() == working);
    // ...and the slot's own order is still what a load of it would bring.
    CHECK(fixture.presets.at(2).order != fixture.view.order().tokens());
}

TEST_CASE("Saving into another slot leaves you where you are, and unsaved")
{
    // Shift+digit and the save button are ONE call aimed at different slots.
    // Aimed elsewhere it stamps a copy and touches nothing about where you
    // are - including that your own slot still differs from the screen, which
    // is what makes the stamp useful rather than a way of leaving.
    Fixture fixture;
    arrangeSomethingElse(fixture);
    REQUIRE(fixture.presets.activeDirty());
    const std::string arranged = fixture.view.stack().tokens();

    CHECK_FALSE(fixture.presets.saveInto(7).status.empty());
    CHECK(fixture.presets.activeSlot() == 1);
    CHECK(fixture.presets.at(7).stack == arranged);
    // Still unsaved: slot 1 is where you are, and it still holds nothing.
    CHECK(fixture.presets.activeDirty());

    // ...and saving where you ARE settles it, by the same call.
    CHECK_FALSE(fixture.presets.saveInto(fixture.presets.activeSlot()).status.empty());
    CHECK_FALSE(fixture.presets.activeDirty());
    CHECK(fixture.presets.at(7).stack == arranged);
}

TEST_CASE("A save with nothing to save writes nothing")
{
    // The dark button and a chord that does nothing are one state. A write
    // producing the same bytes would still spend the preferences file's
    // debounce, so the refusal is in the one place every entry point goes
    // through rather than at each of them.
    Fixture fixture;
    arrangeSomethingElse(fixture);
    REQUIRE_FALSE(fixture.presets.saveInto(fixture.presets.activeSlot()).status.empty());
    REQUIRE_FALSE(fixture.presets.activeDirty());

    const LayoutPresetOutcome again = fixture.presets.saveInto(fixture.presets.activeSlot());
    CHECK(again.status.empty());
    CHECK_FALSE(again.preferencesSaveDue);
}

TEST_CASE("A preset name in a message is quoted, never bare")
{
    // A name is whatever the user typed, so bare in prose it joins our
    // grammar: "Whatever loaded" parses as a sentence about something else,
    // and a preset called "loaded" would read as nonsense either way. The verb
    // leads so the structure is fixed before their words arrive, and the
    // quotes say which words are theirs.
    Fixture fixture;
    const LayoutPresetOutcome renamed = fixture.presets.rename(2, "loaded");
    REQUIRE(renamed.preferencesSaveDue);
    arrangeSomethingElse(fixture);

    CHECK(fixture.presets.saveInto(2).status == "Saved \"loaded\"");
    CHECK(fixture.presets.load(2).status == "Loaded \"loaded\"");

    // A slot never renamed still reads as a name rather than as our own words.
    CHECK(fixture.presets.load(8).status == "Loaded \"Preset 8\"");

    // Clearing the field is not an empty name: the slot goes back to the one
    // it is called by default, so no message is ever left quoting nothing.
    const LayoutPresetOutcome cleared = fixture.presets.rename(2, "   ");
    REQUIRE(cleared.preferencesSaveDue);
    CHECK(fixture.presets.load(2).status == "Loaded \"Preset 2\"");
}

TEST_CASE("A save keeps the name of the slot it lands in")
{
    // A name outlives the layout it was given to, so stamping a layout over a
    // slot replaces what it holds and not what it is called.
    Fixture fixture;
    const LayoutPresetOutcome renamed = fixture.presets.rename(5, "Skin tones");
    REQUIRE(renamed.preferencesSaveDue);
    arrangeSomethingElse(fixture);

    const LayoutPresetOutcome saved = fixture.presets.saveInto(5);
    CHECK(saved.status == "Saved \"Skin tones\"");
    CHECK(presetDisplayName(5, fixture.presets.at(5)) == "Skin tones");
    CHECK_FALSE(fixture.presets.at(5).stack.empty());
}

}  // namespace sidescopes
