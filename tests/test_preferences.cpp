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
    saved.waveform_mode = WaveformMode::RgbAndLuma;
    saved.region = RegionOfInterest{10.0, 20.0, 80.0, 90.0};
    saved.view_mode = 2;
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
    CHECK(loaded.waveform_mode == WaveformMode::RgbAndLuma);
    CHECK(loaded.region.left_percent == 10.0);
    CHECK(loaded.region.bottom_percent == 90.0);
    CHECK(loaded.view_mode == 2);
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
    CHECK(loaded.region.right_percent == 100.0);
    CHECK(loaded.show_graticule);
}

TEST_CASE("Preferences tolerate unknown keys and malformed lines") {
    const auto file = TemporaryFile("forward-compat.txt");
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "future_feature=42\n"
                        << "no separator here\n"
                        << "waveform_gain=0.2\n"
                        << "waveform_mode=99\n";  // out of range: ignored

    const Preferences loaded = LoadPreferences(file);
    CHECK(loaded.waveform_gain == 0.2f);
    CHECK(loaded.waveform_mode == WaveformMode::Rgb);  // default kept
    CHECK(loaded.vectorscope_gain == 3.0f);

    std::filesystem::remove(file);
}

}  // namespace sidescopes
