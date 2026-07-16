#include <catch2/catch_test_macros.hpp>

#include "core/preferences.h"
#include "temp_file.h"

namespace sidescopes {

using namespace test;

TEST_CASE("Preferences round-trip through a file")
{
    Preferences saved;
    saved.vectorscopeGain = 4.5f;
    saved.waveformGain = 0.12f;
    saved.waveformStride = 2;
    saved.vectorscopeSmoothingMs = 60.0f;
    saved.matrix = ChromaMatrix::Bt709;
    saved.traceResponse = TraceResponse::Linear;
    saved.vectorscopeZoom = 2;
    saved.shortcuts.waveform = "X";
    saved.shortcuts.fullRegion = "Q";
    saved.scopeStack = "HWV";  // stacking order is part of the setting
    saved.showGraticule = false;
    saved.windowX = 120;
    saved.windowWidth = 640;

    const TempFile file("roundtrip.txt");
    REQUIRE(savePreferences(saved, file.path()));

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.vectorscopeGain == saved.vectorscopeGain);
    CHECK(loaded.waveformGain == saved.waveformGain);
    CHECK(loaded.waveformStride == saved.waveformStride);
    CHECK(loaded.vectorscopeSmoothingMs == saved.vectorscopeSmoothingMs);
    CHECK(loaded.matrix == ChromaMatrix::Bt709);
    CHECK(loaded.traceResponse == TraceResponse::Linear);
    CHECK(loaded.vectorscopeZoom == 2);
    CHECK(loaded.shortcuts.waveform == "X");
    CHECK(loaded.shortcuts.fullRegion == "Q");
    CHECK(loaded.shortcuts.parade == "R");  // untouched bindings keep defaults
    CHECK(loaded.scopeStack == "HWV");
    CHECK_FALSE(loaded.showGraticule);
    CHECK(loaded.windowX == 120);
    CHECK(loaded.windowWidth == 640);
}

TEST_CASE("Preferences default when the file is missing")
{
    const TempFile file("does-not-exist.txt");
    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.vectorscopeGain == 3.0f);
    CHECK(loaded.waveformGain == 0.05f);
    CHECK(loaded.showGraticule);
}

TEST_CASE("Preferences migrate the legacy single view mode")
{
    const TempFile file("legacy-view-mode.txt");
    file.write("view_mode=2\n");  // the old vectorscope-and-waveform pair

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "VW");
}

TEST_CASE("Preferences migrate the scope bit set and waveform style")
{
    // The RGB+Luma composite folds into the one waveform scope, whose
    // style now lives in the context menu.
    const TempFile file("legacy-bit-set.txt");
    file.write("visible_scopes=6\nwaveform_mode=2\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "WH");
}

TEST_CASE("Preferences fold the retired luma scope into the waveform style")
{
    // A stack saved with the short-lived separate luma waveform: the
    // letter becomes W, the style becomes Luma, the parade stays.
    const TempFile file("legacy-luma-letter.txt");
    file.write("scope_stack=LR\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "WR");
    CHECK(loaded.waveformMode == WaveformMode::Luma);
}

TEST_CASE("Preferences never load an empty scope set")
{
    const TempFile file("empty-scopes.txt");
    file.write("scope_stack=XYZ\n");  // no known scope letters

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "V");
}

TEST_CASE("Preferences deduplicate scope letters")
{
    const TempFile file("dup-scopes.txt");
    file.write("scope_stack=RWRxW\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "RW");
}

TEST_CASE("Preferences keep the color picker in the stack")
{
    const TempFile file("picker-scope.txt");
    file.write("scope_stack=CV\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.scopeStack == "CV");
}

TEST_CASE("Preferences tolerate unknown keys and malformed lines")
{
    const TempFile file("forward-compat.txt");
    file.write(
        "future_feature=42\n"
        "no separator here\n"
        "waveform_gain=0.2\n");

    const Preferences loaded = loadPreferences(file.path());
    CHECK(loaded.waveformGain == 0.2f);
    CHECK(loaded.vectorscopeGain == 3.0f);
}

}  // namespace sidescopes
