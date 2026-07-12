#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "core/preferences.h"

namespace sidescopes {
namespace {

std::filesystem::path TemporaryFile(const char* name) {
    return std::filesystem::temp_directory_path() / "sidescopes-tests" / name;
}

}  // namespace

TEST_CASE("Preferences round-trip through a file") {
    Preferences saved;
    saved.vectorscope_gain = 4.5f;
    saved.waveform_gain = 0.12f;
    saved.waveform_stride = 2;
    saved.vectorscope_smoothing_ms = 60.0f;
    saved.matrix = ChromaMatrix::Bt709;
    saved.scope_stack = "HWV";  // stacking order is part of the setting
    saved.show_graticule = false;
    saved.values_as_percent = false;
    saved.window_x = 120;
    saved.window_width = 640;

    const auto file = TemporaryFile("roundtrip.txt");
    REQUIRE(SavePreferences(saved, file));

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.vectorscope_gain == saved.vectorscope_gain);
    CHECK(loaded.waveform_gain == saved.waveform_gain);
    CHECK(loaded.waveform_stride == saved.waveform_stride);
    CHECK(loaded.vectorscope_smoothing_ms == saved.vectorscope_smoothing_ms);
    CHECK(loaded.matrix == ChromaMatrix::Bt709);
    CHECK(loaded.scope_stack == "HWV");
    CHECK_FALSE(loaded.show_graticule);
    CHECK_FALSE(loaded.values_as_percent);
    CHECK(loaded.window_x == 120);
    CHECK(loaded.window_width == 640);

    std::filesystem::remove(file);
}

TEST_CASE("Preferences default when the file is missing") {
    const Preferences loaded = LoadPreferences(TemporaryFile("does-not-exist.txt"));
    CHECK(loaded.vectorscope_gain == 3.0f);
    CHECK(loaded.waveform_gain == 0.05f);
    CHECK(loaded.show_graticule);
}

TEST_CASE("Preferences migrate the legacy single view mode") {
    const auto file = TemporaryFile("legacy-view-mode.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "view_mode=2\n";  // the old vectorscope-and-waveform pair

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.scope_stack == "VW");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences migrate the scope bit set and waveform style") {
    // The RGB+Luma composite folds into the one waveform scope, whose
    // style now lives in the context menu.
    const auto file = TemporaryFile("legacy-bit-set.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "visible_scopes=6\nwaveform_mode=2\n";

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.scope_stack == "WH");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences fold the retired luma scope into the waveform style") {
    // A stack saved with the short-lived separate luma waveform: the
    // letter becomes W, the style becomes Luma, the parade stays.
    const auto file = TemporaryFile("legacy-luma-letter.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "scope_stack=LR\n";

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.scope_stack == "WR");
    CHECK(loaded.waveform_mode == WaveformMode::Luma);

    std::filesystem::remove(file);
}

TEST_CASE("Preferences never load an empty scope set") {
    const auto file = TemporaryFile("empty-scopes.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "scope_stack=XYZ\n";  // no known scope letters

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.scope_stack == "V");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences deduplicate scope letters") {
    const auto file = TemporaryFile("dup-scopes.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "scope_stack=RWRxW\n";

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.scope_stack == "RW");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences keep the color picker in the stack") {
    const auto file = TemporaryFile("picker-scope.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "scope_stack=CV\n";

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.scope_stack == "CV");

    std::filesystem::remove(file);
}

TEST_CASE("Preferences tolerate unknown keys and malformed lines") {
    const auto file = TemporaryFile("forward-compat.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "future_feature=42\n"
                        << "no separator here\n"
                        << "waveform_gain=0.2\n";

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.waveform_gain == 0.2f);
    CHECK(loaded.vectorscope_gain == 3.0f);

    std::filesystem::remove(file);
}

}  // namespace sidescopes
