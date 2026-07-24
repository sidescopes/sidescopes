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
