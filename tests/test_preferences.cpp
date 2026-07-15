#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "core/preferences.h"

namespace sidescopes {
namespace {

std::filesystem::path temporaryFile(const char* name)
{
    return std::filesystem::temp_directory_path() / "sidescopes-tests" / name;
}

}  // namespace

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
    saved.valuesAsPercent = false;
    saved.windowX = 120;
    saved.windowWidth = 640;

    const auto file = temporaryFile("roundtrip.txt");
    REQUIRE(savePreferences(saved, file));

    const Preferences loaded = loadPreferences(file);
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
    CHECK_FALSE(loaded.valuesAsPercent);
    CHECK(loaded.windowX == 120);
    CHECK(loaded.windowWidth == 640);

    std::filesystem::remove(file);
}

TEST_CASE("Preferences default when the file is missing")
{
    const Preferences loaded = loadPreferences(temporaryFile("does-not-exist.txt"));
    CHECK(loaded.vectorscopeGain == 3.0f);
    CHECK(loaded.waveformGain == 0.05f);
    CHECK(loaded.showGraticule);
}

TEST_CASE("Preferences migrate the legacy single view mode")
{
    const auto file = temporaryFile("legacy-view-mode.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "view_mode=2\n";  // the old vectorscope-and-waveform pair

    const Preferences loaded = loadPreferences(file);
    CHECK(loaded.scopeStack == "VW");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences migrate the scope bit set and waveform style")
{
    // The RGB+Luma composite folds into the one waveform scope, whose
    // style now lives in the context menu.
    const auto file = temporaryFile("legacy-bit-set.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "visible_scopes=6\nwaveform_mode=2\n";

    const Preferences loaded = loadPreferences(file);
    CHECK(loaded.scopeStack == "WH");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences fold the retired luma scope into the waveform style")
{
    // A stack saved with the short-lived separate luma waveform: the
    // letter becomes W, the style becomes Luma, the parade stays.
    const auto file = temporaryFile("legacy-luma-letter.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "scope_stack=LR\n";

    const Preferences loaded = loadPreferences(file);
    CHECK(loaded.scopeStack == "WR");
    CHECK(loaded.waveformMode == WaveformMode::Luma);

    std::filesystem::remove(file);
}

TEST_CASE("Preferences never load an empty scope set")
{
    const auto file = temporaryFile("empty-scopes.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "scope_stack=XYZ\n";  // no known scope letters

    const Preferences loaded = loadPreferences(file);
    CHECK(loaded.scopeStack == "V");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences deduplicate scope letters")
{
    const auto file = temporaryFile("dup-scopes.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "scope_stack=RWRxW\n";

    const Preferences loaded = loadPreferences(file);
    CHECK(loaded.scopeStack == "RW");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences keep the color picker in the stack")
{
    const auto file = temporaryFile("picker-scope.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "scope_stack=CV\n";

    const Preferences loaded = loadPreferences(file);
    CHECK(loaded.scopeStack == "CV");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences tolerate unknown keys and malformed lines")
{
    const auto file = temporaryFile("forward-compat.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "future_feature=42\n"
                        << "no separator here\n"
                        << "waveform_gain=0.2\n";

    const Preferences loaded = loadPreferences(file);
    CHECK(loaded.waveformGain == 0.2f);
    CHECK(loaded.vectorscopeGain == 3.0f);

    std::filesystem::remove(file);
}

}  // namespace sidescopes
