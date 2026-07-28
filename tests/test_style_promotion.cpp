#include <catch2/catch_test_macros.hpp>
#include <string>

#include "core/preferences.h"
#include "temp_file.h"

namespace sidescopes {

using namespace test;

namespace {

constexpr char HistogramId[] = "org.sidescopes.histogram";
constexpr char CombinedHistogramId[] = "org.sidescopes.histogram.combined";

// The value of one scope parameter, or a sentinel when the scope or key is
// unset, so a case reads the map without spelling out both lookups.
double param(const Preferences& preferences, const char* id, const char* key, double missing = -1.0)
{
    const auto scope = preferences.scopeParams.find(id);
    if (scope == preferences.scopeParams.end()) {
        return missing;
    }
    const auto value = scope->second.find(key);

    return value != scope->second.end() ? value->second : missing;
}

// A file holding @p contents, loaded. Every case here goes through the real
// loader: the promotion is the last thing it does, and reading it any other
// way would prove something about a function rather than about a file.
Preferences loadWritten(const TempFile& file, const std::string& contents)
{
    file.write(contents);

    return loadPreferences(file.path());
}

}  // namespace

TEST_CASE("A histogram saved combined comes back as its own scope")
{
    const TempFile file("promote-combined.txt");
    const Preferences loaded = loadWritten(file, "scope_stack=VH\norg.sidescopes.histogram.style=1\n");

    CHECK(loaded.scopeStack == "VG");
}

TEST_CASE("A histogram saved per channel keeps the letter it had")
{
    // The default, and the case that decides whether an untouched install sees
    // any change at all: it must see none, whether the file states the style
    // or leaves it out entirely.
    const TempFile stated("promote-per-channel.txt");
    CHECK(loadWritten(stated, "scope_stack=VH\norg.sidescopes.histogram.style=0\n").scopeStack == "VH");

    const TempFile silent("promote-no-style.txt");
    CHECK(loadWritten(silent, "scope_stack=VH\n").scopeStack == "VH");
}

TEST_CASE("The saved menu order follows a promoted scope")
{
    // The order names scopes whether or not they are on screen, so it carries
    // the retired letter too and has to be rewritten by the same reading.
    const TempFile file("promote-order.txt");
    const Preferences loaded =
        loadWritten(file, "scope_stack=V\nscope_order=HWVRC\norg.sidescopes.histogram.style=1\n");

    CHECK(loaded.scopeOrder == "GWVRC");
}

TEST_CASE("A promoted pane weight follows its scope")
{
    const TempFile file("promote-weights.txt");
    const Preferences loaded = loadWritten(
        file, "scope_stack=VH\nlayout_weights=org.sidescopes.histogram:2.5\norg.sidescopes.histogram.style=1\n");

    CHECK(loaded.layoutWeights.count(HistogramId) == 0);
    CHECK(loaded.layoutWeights.at(CombinedHistogramId) == 2.5);
}

TEST_CASE("A promoted shortcut override follows its scope")
{
    const TempFile file("promote-shortcut.txt");
    const Preferences loaded =
        loadWritten(file, "scope_stack=VH\nshortcut_org.sidescopes.histogram=K\norg.sidescopes.histogram.style=1\n");

    CHECK(loaded.scopeShortcuts.count(HistogramId) == 0);
    CHECK(loaded.scopeShortcuts.at(CombinedHistogramId) == "K");
}

TEST_CASE("A preset is promoted by the style it saved, not the live one")
{
    // The load-bearing case: a preset captures its own choice values, so two
    // slots saved under different styles must migrate differently in one file,
    // and neither by whatever the live scope happens to be set to.
    const TempFile file("promote-presets.txt");
    const Preferences loaded = loadWritten(file,
                                           "scope_stack=VH\n"
                                           "org.sidescopes.histogram.style=1\n"
                                           "layout.preset1.stack=VH\n"
                                           "layout.preset1.weights=org.sidescopes.histogram:3\n"
                                           "layout.preset1.styles=org.sidescopes.histogram.style:0\n"
                                           "layout.preset2.stack=VH\n"
                                           "layout.preset2.styles=org.sidescopes.histogram.style:1\n");

    // Slot 1 was saved per channel: it keeps H, and its weight stays where it
    // was, even though the live histogram is the combined one.
    CHECK(loaded.layoutPresets[0].stack == "VH");
    CHECK(loaded.layoutPresets[0].weights.at(HistogramId) == 3.0);
    // Slot 2 was saved combined.
    CHECK(loaded.layoutPresets[1].stack == "VG");
    // And the retired choice is gone from both, so neither can be read again.
    CHECK(loaded.layoutPresets[0].styles.count(HistogramId) == 0);
    CHECK(loaded.layoutPresets[1].styles.count(HistogramId) == 0);
}

TEST_CASE("A preset saved before styles were captured reads the live one")
{
    const TempFile file("promote-preset-styleless.txt");
    const Preferences loaded = loadWritten(file,
                                           "scope_stack=VH\n"
                                           "org.sidescopes.histogram.style=1\n"
                                           "layout.preset1.stack=VH\n");

    CHECK(loaded.layoutPresets[0].stack == "VG");
}

TEST_CASE("A promoted scope inherits what its sibling was tuned with")
{
    const TempFile file("promote-inherit.txt");
    const Preferences loaded =
        loadWritten(file, "scope_stack=VH\norg.sidescopes.histogram.stride=4\norg.sidescopes.histogram.style=1\n");

    CHECK(param(loaded, CombinedHistogramId, "stride") == 4.0);
}

TEST_CASE("A file the promotion has already read is not promoted again")
{
    // The whole reason the retired key is erased as it is read. Left in the
    // file it would be read again on the next load - and the second reading
    // would fold a histogram the user had since added back onto its sibling,
    // silently changing an arrangement they built.
    const TempFile file("promote-once.txt");
    const Preferences first = loadWritten(file, "scope_stack=VH\norg.sidescopes.histogram.style=1\n");
    REQUIRE(first.scopeStack == "VG");
    REQUIRE(savePreferences(first, file.path()));

    // The user now stacks the per-channel histogram alongside it and saves.
    Preferences edited = loadPreferences(file.path());
    CHECK(edited.scopeStack == "VG");
    edited.scopeStack = "VGH";
    REQUIRE(savePreferences(edited, file.path()));

    CHECK(loadPreferences(file.path()).scopeStack == "VGH");
}

}  // namespace sidescopes
