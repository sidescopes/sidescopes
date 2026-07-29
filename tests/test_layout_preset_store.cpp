#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "app/layout_preset_store.h"
#include "core/preferences.h"

namespace sidescopes {
namespace {

// A preset distinct enough that any one field going missing shows.
LayoutPreset presetOf(std::string stack, int orientation)
{
    LayoutPreset preset;
    preset.stack = std::move(stack);
    preset.orientation = orientation;
    preset.weights = {{"org.sidescopes.vectorscope", 1.5}, {"org.sidescopes.waveform", 0.5}};
    preset.styles = {{"org.sidescopes.waveform", {{"mode", 1.0}}}};

    return preset;
}

// What a slot holding nothing restores. The store never looks inside it, so
// any distinct preset serves; the host builds the real one from the registry.
const LayoutPreset& defaults()
{
    static const LayoutPreset instance = presetOf("V", 0);

    return instance;
}

}  // namespace

TEST_CASE("A fresh preset store holds nine empty slots and opens on the first")
{
    const LayoutPresetStore store;

    // There is no "no preset": a first run is on slot 1, saveable like any
    // other, rather than in a state reachable only by never having used one.
    CHECK(store.activeSlot() == 1);
    CHECK(store.all().size() == 9);
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        CHECK(store.at(slot).stack.empty());
    }
    // An empty slot restores the defaults, which is what makes it loadable.
    CHECK(sameLayout(store.effective(1, defaults()), defaults()));
}

TEST_CASE("A slot holding nothing restores the defaults under its own name")
{
    LayoutPresetStore store;
    store.rename(4, "Skin tones");

    const LayoutPreset restored = store.effective(4, defaults());
    CHECK(restored.stack == defaults().stack);
    CHECK(restored.orientation == defaults().orientation);
    CHECK(restored.weights == defaults().weights);
    CHECK(restored.styles == defaults().styles);
    // The name is the slot's, not the defaults': a slot may be named before
    // anything is saved into it, and loading it must not rename it.
    CHECK(presetDisplayName(4, restored) == "Skin tones");

    // A slot that holds something is returned as it is, defaults untouched.
    store.store(4, presetOf("WH", 2));
    CHECK(store.effective(4, defaults()).stack == "WH");
}

TEST_CASE("Storing a preset leaves which slot is active alone")
{
    // A write is not a move. The live layout writes itself into the slot it is
    // already on, and Shift+digit writes into one it is deliberately NOT on -
    // so storing must never be the thing that decides where you are.
    LayoutPresetStore store;
    store.store(3, presetOf("VW", 2));

    CHECK(store.activeSlot() == 1);
    CHECK(store.at(3).stack == "VW");
    CHECK(store.at(3).orientation == 2);
    // The other slots are untouched.
    CHECK(store.at(1).stack.empty());
    CHECK(store.at(9).stack.empty());
}

TEST_CASE("Two presets differ if any captured field does")
{
    // What decides whether the live layout has to be written back into its
    // slot. Styles and weights are the two a shallow comparison would miss,
    // and missing one means a change the user made is quietly not kept.
    const LayoutPreset saved = presetOf("VW", 2);
    CHECK(sameLayout(saved, presetOf("VW", 2)));

    LayoutPreset stack = saved;
    stack.stack = "VWH";
    CHECK_FALSE(sameLayout(saved, stack));

    LayoutPreset orientation = saved;
    orientation.orientation = 1;
    CHECK_FALSE(sameLayout(saved, orientation));

    LayoutPreset weights = saved;
    weights.weights["org.sidescopes.waveform"] = 0.75;
    CHECK_FALSE(sameLayout(saved, weights));

    LayoutPreset styles = saved;
    styles.styles["org.sidescopes.waveform"]["mode"] = 2.0;
    CHECK_FALSE(sameLayout(saved, styles));

    // A name is not part of the layout: it belongs to the slot, so renaming
    // one must not read as a change that needs writing back.
    LayoutPreset named = saved;
    named.name = "Skin tones";
    CHECK(sameLayout(saved, named));
}

TEST_CASE("Loading a slot makes it active without touching what is stored")
{
    LayoutPresetStore store;
    store.store(2, presetOf("V", 1));
    store.store(5, presetOf("WH", 2));
    REQUIRE(store.activeSlot() == 1);

    store.markLoaded(2);
    CHECK(store.activeSlot() == 2);
    CHECK(store.at(2).stack == "V");
    CHECK(store.at(5).stack == "WH");
}

TEST_CASE("A slot goes by its number until it is given a name")
{
    LayoutPreset preset;
    CHECK(presetDisplayName(1, preset) == "Preset 1");
    CHECK(presetDisplayName(9, preset) == "Preset 9");

    preset.name = "Portrait check";
    CHECK(presetDisplayName(1, preset) == "Portrait check");
}

TEST_CASE("Renaming a slot leaves what it holds alone")
{
    LayoutPresetStore store;
    store.store(2, presetOf("VW", 2));
    store.markLoaded(2);
    REQUIRE(store.activeSlot() == 2);

    store.rename(2, "Portrait check");
    CHECK(presetDisplayName(2, store.at(2)) == "Portrait check");
    CHECK(store.at(2).stack == "VW");
    // A rename is not a load: which slot is active is untouched.
    CHECK(store.activeSlot() == 2);
}

TEST_CASE("An empty slot can be named before it holds anything")
{
    // Naming a set of slots up front is the point of naming them at all, so a
    // name does not wait for a layout - and does not conjure one either.
    LayoutPresetStore store;
    store.rename(4, "Skin tones");

    CHECK(presetDisplayName(4, store.at(4)) == "Skin tones");
    CHECK(store.at(4).stack.empty());
    // A rename is not a load, so it does not move which slot is active.
    CHECK(store.activeSlot() == 1);
}

TEST_CASE("Saving over a named slot keeps its name")
{
    // What the live view captures carries no name, so a save that took the
    // capture wholesale would wipe the name every time the slot was updated.
    LayoutPresetStore store;
    store.rename(3, "Portrait check");
    store.store(3, presetOf("VWH", 1));

    CHECK(presetDisplayName(3, store.at(3)) == "Portrait check");
    CHECK(store.at(3).stack == "VWH");
}

TEST_CASE("A blank name puts a slot back on its default")
{
    LayoutPresetStore store;
    store.rename(5, "Skin tones");
    REQUIRE(store.at(5).name == "Skin tones");

    store.rename(5, "   ");
    CHECK(store.at(5).name.empty());
    CHECK(presetDisplayName(5, store.at(5)) == "Preset 5");
}

TEST_CASE("A name is cleaned on the way into a slot")
{
    // The store is the one door a typed name comes through, so it is where the
    // file's rules are applied rather than at the moment of writing.
    LayoutPresetStore store;
    store.rename(1, "  Portrait\ncheck  ");
    CHECK(store.at(1).name == "Portraitcheck");
}

TEST_CASE("Restoring replaces every slot and the active one")
{
    LayoutPresetStore store;
    store.store(1, presetOf("V", 1));

    std::array<LayoutPreset, LayoutPresetSlots> presets;
    presets[0] = presetOf("WH", 2);
    presets[8] = presetOf("R", 1);
    store.restore(presets, 9);

    CHECK(store.activeSlot() == 9);
    CHECK(store.at(1).stack == "WH");
    CHECK(store.at(9).stack == "R");
}

TEST_CASE("Restoring an active slot outside the nine opens on the first")
{
    // A file written before the application was always on a preset carries 0,
    // and a hand-edited one can carry anything. Neither may leave the store
    // pointing at a slot that does not exist.
    LayoutPresetStore store;
    const std::array<LayoutPreset, LayoutPresetSlots> presets;

    store.restore(presets, 0);
    CHECK(store.activeSlot() == 1);

    store.restore(presets, LayoutPresetSlots + 1);
    CHECK(store.activeSlot() == 1);

    store.restore(presets, -3);
    CHECK(store.activeSlot() == 1);

    // The nine themselves survive untouched.
    store.restore(presets, LayoutPresetSlots);
    CHECK(store.activeSlot() == LayoutPresetSlots);
}

}  // namespace sidescopes
