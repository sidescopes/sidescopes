#include "core/preferences.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

namespace sidescopes {
namespace {

std::map<std::string, std::string, std::less<>> ParseKeyValueLines(std::istream& input) {
    std::map<std::string, std::string, std::less<>> values;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        values[line.substr(0, separator)] = line.substr(separator + 1);
    }
    return values;
}

}  // namespace

Preferences LoadPreferences(const std::filesystem::path& file) {
    Preferences preferences;
    std::ifstream input(file);
    if (!input) return preferences;

    const auto values = ParseKeyValueLines(input);
    const auto read_float = [&](const char* key, float& out) {
        if (const auto found = values.find(key); found != values.end())
            out = std::strtof(found->second.c_str(), nullptr);
    };
    const auto read_int = [&](const char* key, int& out) {
        if (const auto found = values.find(key); found != values.end())
            out = static_cast<int>(std::strtol(found->second.c_str(), nullptr, 10));
    };
    const auto read_bool = [&](const char* key, bool& out) {
        if (const auto found = values.find(key); found != values.end()) out = found->second == "1";
    };

    read_float("vectorscope_gain", preferences.vectorscope_gain);
    read_float("waveform_gain", preferences.waveform_gain);
    read_int("vectorscope_stride", preferences.vectorscope_stride);
    read_int("waveform_stride", preferences.waveform_stride);
    read_float("histogram_gain", preferences.histogram_gain);
    read_int("histogram_stride", preferences.histogram_stride);
    read_float("vectorscope_smoothing_ms", preferences.vectorscope_smoothing_ms);
    read_float("waveform_smoothing_ms", preferences.waveform_smoothing_ms);
    int matrix = 0;
    read_int("matrix", matrix);
    preferences.matrix = matrix == 1 ? ChromaMatrix::Bt709 : ChromaMatrix::Bt601;
    // Two generations of legacy scope keys fold into the stack: the
    // single view_mode became the visible_scopes bit set, and the bit set
    // plus the waveform style became the ordered letter stack.
    int legacy_view_mode = -1;
    read_int("view_mode", legacy_view_mode);
    int visible_scopes = 1;
    if (legacy_view_mode >= 0 && legacy_view_mode <= 3) {
        constexpr int kLegacyScopes[4] = {1, 2, 3, 4};
        visible_scopes = kLegacyScopes[legacy_view_mode];
    }
    read_int("visible_scopes", visible_scopes);
    int waveform_mode = static_cast<int>(WaveformMode::Rgb);
    read_int("waveform_mode", waveform_mode);
    std::string stack;
    if (visible_scopes & 1) stack += 'V';
    if (visible_scopes & 2) {
        switch (static_cast<WaveformMode>(waveform_mode)) {
            case WaveformMode::Luma:
                stack += 'L';
                break;
            case WaveformMode::RgbAndLuma:
                stack += "WL";
                break;
            case WaveformMode::RgbParade:
                stack += 'R';
                break;
            case WaveformMode::Rgb:
            default:
                stack += 'W';
                break;
        }
    }
    if (visible_scopes & 4) stack += 'H';
    if (!stack.empty()) preferences.scope_stack = stack;
    if (const auto found = values.find("scope_stack"); found != values.end())
        preferences.scope_stack = found->second;
    // Whatever the source, keep only known letters, each once, in order;
    // an empty result falls back to the vectorscope.
    std::string cleaned;
    for (const char letter : preferences.scope_stack) {
        if (std::string_view("VWLRH").find(letter) == std::string_view::npos) continue;
        if (cleaned.find(letter) == std::string::npos) cleaned += letter;
    }
    preferences.scope_stack = cleaned.empty() ? "V" : cleaned;
    read_bool("show_graticule", preferences.show_graticule);
    read_bool("values_as_percent", preferences.values_as_percent);
    read_int("window_x", preferences.window_x);
    read_int("window_y", preferences.window_y);
    read_int("window_width", preferences.window_width);
    read_int("window_height", preferences.window_height);
    return preferences;
}

bool SavePreferences(const Preferences& preferences, const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);

    std::ostringstream out;
    out << "vectorscope_gain=" << preferences.vectorscope_gain << '\n'
        << "waveform_gain=" << preferences.waveform_gain << '\n'
        << "vectorscope_stride=" << preferences.vectorscope_stride << '\n'
        << "waveform_stride=" << preferences.waveform_stride << '\n'
        << "histogram_gain=" << preferences.histogram_gain << '\n'
        << "histogram_stride=" << preferences.histogram_stride << '\n'
        << "vectorscope_smoothing_ms=" << preferences.vectorscope_smoothing_ms << '\n'
        << "waveform_smoothing_ms=" << preferences.waveform_smoothing_ms << '\n'
        << "matrix=" << (preferences.matrix == ChromaMatrix::Bt709 ? 1 : 0) << '\n'
        << "scope_stack=" << preferences.scope_stack << '\n'
        << "show_graticule=" << (preferences.show_graticule ? 1 : 0) << '\n'
        << "values_as_percent=" << (preferences.values_as_percent ? 1 : 0) << '\n'
        << "window_x=" << preferences.window_x << '\n'
        << "window_y=" << preferences.window_y << '\n'
        << "window_width=" << preferences.window_width << '\n'
        << "window_height=" << preferences.window_height << '\n';

    std::ofstream output(file, std::ios::trunc);
    if (!output) return false;
    output << out.str();
    return static_cast<bool>(output);
}

}  // namespace sidescopes
