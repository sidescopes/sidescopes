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

void readFloat(const std::map<std::string, std::string, std::less<>>& values, const char* key, float& out)
{
    if (const auto found = values.find(key); found != values.end()) {
        out = std::strtof(found->second.c_str(), nullptr);
    }
}

void readInt(const std::map<std::string, std::string, std::less<>>& values, const char* key, int& out)
{
    if (const auto found = values.find(key); found != values.end()) {
        out = static_cast<int>(std::strtol(found->second.c_str(), nullptr, 10));
    }
}

void readBool(const std::map<std::string, std::string, std::less<>>& values, const char* key, bool& out)
{
    if (const auto found = values.find(key); found != values.end()) {
        out = found->second == "1";
    }
}

// A shortcut binding is a single letter A-Z or "Escape"; anything else is
// left alone so the default survives.
void readShortcut(const std::map<std::string, std::string, std::less<>>& values, const char* key, std::string& out)
{
    const auto found = values.find(key);
    if (found == values.end()) {
        return;
    }
    const std::string& value = found->second;
    const bool letter = value.size() == 1 && value[0] >= 'A' && value[0] <= 'Z';
    if (letter || value == "Escape") {
        out = value;
    }
}

void readShortcuts(const std::map<std::string, std::string, std::less<>>& values, ShortcutBindings& shortcuts)
{
    readShortcut(values, "shortcut_vectorscope", shortcuts.vectorscope);
    readShortcut(values, "shortcut_waveform", shortcuts.waveform);
    readShortcut(values, "shortcut_parade", shortcuts.parade);
    readShortcut(values, "shortcut_histogram", shortcuts.histogram);
    readShortcut(values, "shortcut_color_picker", shortcuts.colorPicker);
    readShortcut(values, "shortcut_pick_window", shortcuts.pickWindow);
    readShortcut(values, "shortcut_draw_region", shortcuts.drawRegion);
    readShortcut(values, "shortcut_pick_faces", shortcuts.pickFaces);
    readShortcut(values, "shortcut_pin_color", shortcuts.pinColor);
    readShortcut(values, "shortcut_vectorscope_zoom", shortcuts.vectorscopeZoom);
    readShortcut(values, "shortcut_full_region", shortcuts.fullRegion);
}

// Decodes the enum-valued and clamped fields (chroma matrix, trace
// response, waveform style, magnify zoom). The one waveform_mode key also
// drives the legacy scope-stack migration, so its raw value is returned to
// be read only once.
int readEnumFields(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    int matrix = preferences.matrix == ChromaMatrix::Bt709 ? 1 : 0;
    readInt(values, "matrix", matrix);
    preferences.matrix = matrix == 0 ? ChromaMatrix::Bt601 : ChromaMatrix::Bt709;

    int traceResponse = preferences.traceResponse == TraceResponse::Linear ? 1 : 0;
    readInt(values, "trace_response", traceResponse);
    preferences.traceResponse = traceResponse == 1 ? TraceResponse::Linear : TraceResponse::Boosted;

    int storedWaveformMode = static_cast<int>(preferences.waveformMode);
    readInt(values, "waveform_mode", storedWaveformMode);
    preferences.waveformMode = storedWaveformMode == static_cast<int>(WaveformMode::Luma) ? WaveformMode::Luma
                               : storedWaveformMode == static_cast<int>(WaveformMode::ColoredLuma)
                                   ? WaveformMode::ColoredLuma
                                   : WaveformMode::Rgb;

    readInt(values, "vectorscope_zoom", preferences.vectorscopeZoom);
    if (preferences.vectorscopeZoom != 2 && preferences.vectorscopeZoom != 4) {
        preferences.vectorscopeZoom = 1;
    }

    return storedWaveformMode;
}

// The oldest builds stored a single view_mode; the next generation stored a
// visible_scopes bit set that supersedes it. Returns the resulting bit set,
// defaulting to the vectorscope alone.
int legacyVisibleScopes(const std::map<std::string, std::string, std::less<>>& values)
{
    int legacyViewMode = -1;
    readInt(values, "view_mode", legacyViewMode);
    int visibleScopes = 1;
    if (legacyViewMode >= 0 && legacyViewMode <= 3) {
        constexpr int LegacyScopes[4] = {1, 2, 3, 4};
        visibleScopes = LegacyScopes[legacyViewMode];
    }
    readInt(values, "visible_scopes", visibleScopes);

    return visibleScopes;
}

// Translates the legacy visible-scopes bit set into ordered stack letters,
// mapping the waveform bit through its stored style.
std::string legacyScopeLetters(int visibleScopes, int storedWaveformMode)
{
    std::string stack;
    if (visibleScopes & 1) {
        stack += 'V';
    }
    if (visibleScopes & 2) {
        switch (static_cast<WaveformMode>(storedWaveformMode)) {
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

    return stack;
}

// Keeps only known scope letters, each once, in order, defaulting to the
// vectorscope. The retired L (a separate luma waveform) becomes the
// waveform in its Luma style, unless an RGB waveform is already stacked,
// which keeps the letter.
std::string cleanedScopeStack(const std::string& stack, Preferences& preferences)
{
    const bool hadWaveform = stack.find('W') != std::string::npos;
    std::string cleaned;
    for (char letter : stack) {
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

    return cleaned.empty() ? "V" : cleaned;
}

// Folds two generations of legacy scope keys into scopeStack: the single
// view_mode, then the visible_scopes bit set plus the stored waveform
// style, with the scope_stack key superseding both when present.
void migrateScopeStack(const std::map<std::string, std::string, std::less<>>& values, int storedWaveformMode,
                       Preferences& preferences)
{
    const int visibleScopes = legacyVisibleScopes(values);
    const std::string stack = legacyScopeLetters(visibleScopes, storedWaveformMode);
    if (!stack.empty()) {
        preferences.scopeStack = stack;
    }
    if (const auto found = values.find("scope_stack"); found != values.end()) {
        preferences.scopeStack = found->second;
    }

    preferences.scopeStack = cleanedScopeStack(preferences.scopeStack, preferences);
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
    readFloat(values, "vectorscope_gain", preferences.vectorscopeGain);
    readFloat(values, "waveform_gain", preferences.waveformGain);
    readInt(values, "vectorscope_stride", preferences.vectorscopeStride);
    readInt(values, "waveform_stride", preferences.waveformStride);
    readInt(values, "histogram_stride", preferences.histogramStride);
    readFloat(values, "vectorscope_smoothing_ms", preferences.vectorscopeSmoothingMs);
    readFloat(values, "waveform_smoothing_ms", preferences.waveformSmoothingMs);

    const int storedWaveformMode = readEnumFields(values, preferences);
    readBool(values, "histogram_per_channel", preferences.histogramPerChannel);
    migrateScopeStack(values, storedWaveformMode, preferences);
    readBool(values, "show_graticule", preferences.showGraticule);

    readShortcuts(values, preferences.shortcuts);

    readInt(values, "window_x", preferences.windowX);
    readInt(values, "window_y", preferences.windowY);
    readInt(values, "window_width", preferences.windowWidth);
    readInt(values, "window_height", preferences.windowHeight);

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
