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

}  // namespace

TEST_CASE("A fresh preset store holds nine empty slots and no active one")
{
    const LayoutPresetStore store;

    CHECK(store.activeSlot() == 0);
    CHECK(store.all().size() == 9);
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        CHECK(store.at(slot).stack.empty());
    }
    // Nothing is active, so nothing can have drifted.
    CHECK_FALSE(store.isDirty(presetOf("V", 1)));
}

TEST_CASE("Saving a preset stores it and makes its slot active")
{
    LayoutPresetStore store;
    store.save(3, presetOf("VW", 2));

    CHECK(store.activeSlot() == 3);
    CHECK(store.at(3).stack == "VW");
    CHECK(store.at(3).orientation == 2);
    // The other slots are untouched.
    CHECK(store.at(1).stack.empty());
    CHECK(store.at(9).stack.empty());
}

TEST_CASE("The active slot is dirty as soon as the live layout differs")
{
    LayoutPresetStore store;
    const LayoutPreset saved = presetOf("VW", 2);
    store.save(4, saved);

    // What was saved is what is live: the chip carries no star.
    CHECK_FALSE(store.isDirty(saved));

    // Each captured field on its own is enough to have drifted, styles and
    // weights included - the two a shallow comparison would miss.
    LayoutPreset stack = saved;
    stack.stack = "VWH";
    CHECK(store.isDirty(stack));

    LayoutPreset orientation = saved;
    orientation.orientation = 1;
    CHECK(store.isDirty(orientation));

    LayoutPreset weights = saved;
    weights.weights["org.sidescopes.waveform"] = 0.75;
    CHECK(store.isDirty(weights));

    LayoutPreset styles = saved;
    styles.styles["org.sidescopes.waveform"]["mode"] = 2.0;
    CHECK(store.isDirty(styles));
}

TEST_CASE("Loading a slot makes it active without touching what is stored")
{
    LayoutPresetStore store;
    store.save(2, presetOf("V", 1));
    store.save(5, presetOf("WH", 2));
    REQUIRE(store.activeSlot() == 5);

    store.markLoaded(2);
    CHECK(store.activeSlot() == 2);
    CHECK(store.at(2).stack == "V");
    CHECK(store.at(5).stack == "WH");
    // The live layout is what slot 2 holds, so nothing has drifted yet.
    CHECK_FALSE(store.isDirty(presetOf("V", 1)));
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
    store.save(2, presetOf("VW", 2));
    REQUIRE(store.activeSlot() == 2);

    store.rename(2, "Portrait check");
    CHECK(presetDisplayName(2, store.at(2)) == "Portrait check");
    CHECK(store.at(2).stack == "VW");
    // A rename is not a load: which slot is active is untouched.
    CHECK(store.activeSlot() == 2);
    // Nor is it a drift of the live layout, which still matches what is stored.
    CHECK_FALSE(store.isDirty(presetOf("VW", 2)));
}

TEST_CASE("An empty slot can be named before it holds anything")
{
    // Naming a set of slots up front is the point of naming them at all, so a
    // name does not wait for a layout - and does not conjure one either.
    LayoutPresetStore store;
    store.rename(4, "Skin tones");

    CHECK(presetDisplayName(4, store.at(4)) == "Skin tones");
    CHECK(store.at(4).stack.empty());
    CHECK(store.activeSlot() == 0);
}

TEST_CASE("Saving over a named slot keeps its name")
{
    // What the live view captures carries no name, so a save that took the
    // capture wholesale would wipe the name every time the slot was updated.
    LayoutPresetStore store;
    store.rename(3, "Portrait check");
    store.save(3, presetOf("VWH", 1));

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
    store.save(1, presetOf("V", 1));

    std::array<LayoutPreset, LayoutPresetSlots> presets;
    presets[0] = presetOf("WH", 2);
    presets[8] = presetOf("R", 1);
    store.restore(presets, 9);

    CHECK(store.activeSlot() == 9);
    CHECK(store.at(1).stack == "WH");
    CHECK(store.at(9).stack == "R");
    CHECK(store.isDirty(presetOf("V", 1)));
}

}  // namespace sidescopes
