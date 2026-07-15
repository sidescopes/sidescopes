#include "core/preferences.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

namespace sidescopes {
namespace {

std::map<std::string, std::string, std::less<>> parseKeyValueLines(std::istream& input)
{
    std::map<std::string, std::string, std::less<>> values;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values[line.substr(0, separator)] = line.substr(separator + 1);
    }
    return values;
}

}  // namespace

Preferences loadPreferences(const std::filesystem::path& file)
{
    Preferences preferences;
    std::ifstream input(file);
    if (!input) {
        return preferences;
    }

    const auto values = parseKeyValueLines(input);
    const auto readFloat = [&](const char* key, float& out) {
        if (const auto found = values.find(key); found != values.end()) {
            out = std::strtof(found->second.c_str(), nullptr);
        }
    };
    const auto readInt = [&](const char* key, int& out) {
        if (const auto found = values.find(key); found != values.end()) {
            out = static_cast<int>(std::strtol(found->second.c_str(), nullptr, 10));
        }
    };
    const auto readBool = [&](const char* key, bool& out) {
        if (const auto found = values.find(key); found != values.end()) {
            out = found->second == "1";
        }
    };

    readFloat("vectorscope_gain", preferences.vectorscopeGain);
    readFloat("waveform_gain", preferences.waveformGain);
    readInt("vectorscope_stride", preferences.vectorscopeStride);
    readInt("waveform_stride", preferences.waveformStride);
    readInt("histogram_stride", preferences.histogramStride);
    readFloat("vectorscope_smoothing_ms", preferences.vectorscopeSmoothingMs);
    readFloat("waveform_smoothing_ms", preferences.waveformSmoothingMs);
    int matrix = 1;
    readInt("matrix", matrix);
    preferences.matrix = matrix == 0 ? ChromaMatrix::Bt601 : ChromaMatrix::Bt709;
    int traceResponse = 0;
    readInt("trace_response", traceResponse);
    preferences.traceResponse = traceResponse == 1 ? TraceResponse::Linear : TraceResponse::Boosted;
    int waveformStyle = static_cast<int>(preferences.waveformMode);
    readInt("waveform_mode", waveformStyle);
    preferences.waveformMode = waveformStyle == static_cast<int>(WaveformMode::Luma) ? WaveformMode::Luma
                               : waveformStyle == static_cast<int>(WaveformMode::ColoredLuma)
                                   ? WaveformMode::ColoredLuma
                                   : WaveformMode::Rgb;
    readBool("histogram_per_channel", preferences.histogramPerChannel);
    // Two generations of legacy scope keys fold into the stack: the
    // single view_mode became the visible_scopes bit set, and the bit set
    // plus the waveform style became the ordered letter stack.
    int legacyViewMode = -1;
    readInt("view_mode", legacyViewMode);
    int visibleScopes = 1;
    if (legacyViewMode >= 0 && legacyViewMode <= 3) {
        constexpr int LegacyScopes[4] = {1, 2, 3, 4};
        visibleScopes = LegacyScopes[legacyViewMode];
    }
    readInt("visible_scopes", visibleScopes);
    int waveformMode = static_cast<int>(WaveformMode::Rgb);
    readInt("waveform_mode", waveformMode);
    std::string stack;
    if (visibleScopes & 1) {
        stack += 'V';
    }
    if (visibleScopes & 2) {
        switch (static_cast<WaveformMode>(waveformMode)) {
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
    if (visibleScopes & 4) {
        stack += 'H';
    }
    if (!stack.empty()) {
        preferences.scopeStack = stack;
    }
    if (const auto found = values.find("scope_stack"); found != values.end()) {
        preferences.scopeStack = found->second;
    }
    // Whatever the source, keep only known letters, each once, in order;
    // an empty result falls back to the vectorscope. The retired L (a
    // separate luma waveform scope) becomes the waveform in its Luma
    // style - unless an RGB waveform was stacked alongside, which wins
    // the letter.
    const bool hadWaveform = preferences.scopeStack.find('W') != std::string::npos;
    std::string cleaned;
    for (char letter : preferences.scopeStack) {
        if (letter == 'L') {
            if (hadWaveform) {
                continue;
            }
            letter = 'W';
            preferences.waveformMode = WaveformMode::Luma;
        }
        if (std::string_view("VWRHC").find(letter) == std::string_view::npos) {
            continue;
        }
        if (cleaned.find(letter) == std::string::npos) {
            cleaned += letter;
        }
    }
    preferences.scopeStack = cleaned.empty() ? "V" : cleaned;
    readBool("show_graticule", preferences.showGraticule);
    readInt("vectorscope_zoom", preferences.vectorscopeZoom);
    if (preferences.vectorscopeZoom != 2 && preferences.vectorscopeZoom != 4) {
        preferences.vectorscopeZoom = 1;
    }
    readBool("values_as_percent", preferences.valuesAsPercent);
    const auto readShortcut = [&](const char* key, std::string& out) {
        const auto found = values.find(key);
        if (found == values.end()) {
            return;
        }
        const std::string& value = found->second;
        const bool letter = value.size() == 1 && value[0] >= 'A' && value[0] <= 'Z';
        if (letter || value == "Escape") {
            out = value;
        }
    };
    readShortcut("shortcut_vectorscope", preferences.shortcuts.vectorscope);
    readShortcut("shortcut_waveform", preferences.shortcuts.waveform);
    readShortcut("shortcut_parade", preferences.shortcuts.parade);
    readShortcut("shortcut_histogram", preferences.shortcuts.histogram);
    readShortcut("shortcut_color_picker", preferences.shortcuts.colorPicker);
    readShortcut("shortcut_pick_window", preferences.shortcuts.pickWindow);
    readShortcut("shortcut_draw_region", preferences.shortcuts.drawRegion);
    readShortcut("shortcut_pick_faces", preferences.shortcuts.pickFaces);
    readShortcut("shortcut_pin_color", preferences.shortcuts.pinColor);
    readShortcut("shortcut_vectorscope_zoom", preferences.shortcuts.vectorscopeZoom);
    readShortcut("shortcut_full_region", preferences.shortcuts.fullRegion);
    readInt("window_x", preferences.windowX);
    readInt("window_y", preferences.windowY);
    readInt("window_width", preferences.windowWidth);
    readInt("window_height", preferences.windowHeight);
    return preferences;
}

bool savePreferences(const Preferences& preferences, const std::filesystem::path& file)
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);

    std::ostringstream out;
    out << "vectorscope_gain=" << preferences.vectorscopeGain << '\n'
        << "waveform_gain=" << preferences.waveformGain << '\n'
        << "vectorscope_stride=" << preferences.vectorscopeStride << '\n'
        << "waveform_stride=" << preferences.waveformStride << '\n'
        << "histogram_stride=" << preferences.histogramStride << '\n'
        << "vectorscope_smoothing_ms=" << preferences.vectorscopeSmoothingMs << '\n'
        << "waveform_smoothing_ms=" << preferences.waveformSmoothingMs << '\n'
        << "matrix=" << (preferences.matrix == ChromaMatrix::Bt709 ? 1 : 0) << '\n'
        << "trace_response=" << (preferences.traceResponse == TraceResponse::Linear ? 1 : 0) << '\n'
        << "scope_stack=" << preferences.scopeStack << '\n'
        << "waveform_mode=" << static_cast<int>(preferences.waveformMode) << '\n'
        << "histogram_per_channel=" << (preferences.histogramPerChannel ? 1 : 0) << '\n'
        << "show_graticule=" << (preferences.showGraticule ? 1 : 0) << '\n'
        << "vectorscope_zoom=" << preferences.vectorscopeZoom << '\n'
        << "values_as_percent=" << (preferences.valuesAsPercent ? 1 : 0) << '\n'
        << "window_x=" << preferences.windowX << '\n'
        << "window_y=" << preferences.windowY << '\n'
        << "window_width=" << preferences.windowWidth << '\n'
        << "window_height=" << preferences.windowHeight << '\n'
        << "shortcut_vectorscope=" << preferences.shortcuts.vectorscope << '\n'
        << "shortcut_waveform=" << preferences.shortcuts.waveform << '\n'
        << "shortcut_parade=" << preferences.shortcuts.parade << '\n'
        << "shortcut_histogram=" << preferences.shortcuts.histogram << '\n'
        << "shortcut_color_picker=" << preferences.shortcuts.colorPicker << '\n'
        << "shortcut_pick_window=" << preferences.shortcuts.pickWindow << '\n'
        << "shortcut_draw_region=" << preferences.shortcuts.drawRegion << '\n'
        << "shortcut_pick_faces=" << preferences.shortcuts.pickFaces << '\n'
        << "shortcut_pin_color=" << preferences.shortcuts.pinColor << '\n'
        << "shortcut_vectorscope_zoom=" << preferences.shortcuts.vectorscopeZoom << '\n'
        << "shortcut_full_region=" << preferences.shortcuts.fullRegion << '\n';

    std::ofstream output(file, std::ios::trunc);
    if (!output) {
        return false;
    }
    output << out.str();
    return static_cast<bool>(output);
}

}  // namespace sidescopes
