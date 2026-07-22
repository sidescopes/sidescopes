#include "core/preferences.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#include "core/scopes/scope_types.h"

namespace sidescopes {
namespace {

// The built-in scope ids the file format knows for legacy migration; new files
// carry them as generic scopeId.paramKey keys the loader parses blind.
constexpr char VectorscopeId[] = "org.sidescopes.vectorscope";
constexpr char WaveformId[] = "org.sidescopes.waveform";
constexpr char ParadeId[] = "org.sidescopes.parade";
constexpr char HistogramId[] = "org.sidescopes.histogram";
constexpr char ColorPickerId[] = "org.sidescopes.colorpicker";

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

// Reads a legacy numeric key straight into a scope parameter slot, leaving the
// slot's default when the key is absent.
void readLegacyDouble(const std::map<std::string, std::string, std::less<>>& values, const char* key,
                      std::map<std::string, double>& params, const char* paramKey)
{
    if (const auto found = values.find(key); found != values.end()) {
        params[paramKey] = std::strtod(found->second.c_str(), nullptr);
    }
}

// A shortcut binding is a single letter A-Z or "Escape"; anything else is
// rejected so an unusable chord never lands in a binding.
bool validBinding(const std::string& value)
{
    const bool letter = value.size() == 1 && value[0] >= 'A' && value[0] <= 'Z';

    return letter || value == "Escape";
}

// Reads a validated binding, leaving the default when the key is absent or the
// value is not a usable chord.
void readShortcut(const std::map<std::string, std::string, std::less<>>& values, const char* key, std::string& out)
{
    if (const auto found = values.find(key); found != values.end() && validBinding(found->second)) {
        out = found->second;
    }
}

// The action bindings, keyed by their own names; the scope-toggle bindings are
// keyed by scope id and read separately into the map.
void readShortcuts(const std::map<std::string, std::string, std::less<>>& values, ShortcutBindings& shortcuts)
{
    readShortcut(values, "shortcut_attach_window", shortcuts.attachWindow);
    readShortcut(values, "shortcut_draw_area", shortcuts.drawArea);
    readShortcut(values, "shortcut_select_face", shortcuts.selectFace);
    readShortcut(values, "shortcut_pin_color", shortcuts.pinColor);
    readShortcut(values, "shortcut_vectorscope_zoom", shortcuts.vectorscopeZoom);
    readShortcut(values, "shortcut_full_screen", shortcuts.fullScreen);
}

// Reads the scope-toggle bindings into the map keyed by scope id. Each retired
// per-name key folds onto its scope id first; a validated new shortcut_<id> key
// supersedes it. Only overrides are stored; every other scope defaults to its
// letter, which the host resolves.
void readScopeShortcuts(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    constexpr std::pair<const char*, const char*> Legacy[] = {
        {"shortcut_vectorscope", VectorscopeId},
        {"shortcut_waveform", WaveformId},
        {"shortcut_parade", ParadeId},
        {"shortcut_histogram", HistogramId},
        {"shortcut_color_picker", ColorPickerId},
    };
    for (const auto& [key, id] : Legacy) {
        if (const auto found = values.find(key); found != values.end() && validBinding(found->second)) {
            preferences.scopeShortcuts[id] = found->second;
        }
    }

    constexpr std::string_view NewPrefix = "shortcut_org.";
    constexpr std::string_view KeyPrefix = "shortcut_";
    for (const auto& [key, value] : values) {
        if (key.rfind(NewPrefix, 0) == 0 && validBinding(value)) {
            preferences.scopeShortcuts[key.substr(KeyPrefix.size())] = value;
        }
    }
}

// The retired waveform_mode enum int maps to the waveform module's style
// choice: 0 RGB, 1 Luma, 2 Luma (Colored). Only Luma and ColoredLuma had
// dedicated codes; every other mode read as RGB.
double waveformModeChoice(long storedMode)
{
    if (storedMode == static_cast<int>(WaveformMode::Luma)) {
        return 1.0;
    }
    if (storedMode == static_cast<int>(WaveformMode::ColoredLuma)) {
        return 2.0;
    }

    return 0.0;
}

// Folds the retired per-scope keys into the generic map. A matching generic
// key read afterwards supersedes any value set here.
void readLegacyScopeParams(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    std::map<std::string, double>& vectorscope = preferences.scopeParams[VectorscopeId];
    std::map<std::string, double>& waveform = preferences.scopeParams[WaveformId];
    std::map<std::string, double>& histogram = preferences.scopeParams[HistogramId];

    readLegacyDouble(values, "vectorscope_gain", vectorscope, "gain");
    readLegacyDouble(values, "vectorscope_stride", vectorscope, "stride");
    readLegacyDouble(values, "vectorscope_smoothing_ms", vectorscope, "smoothing_ms");
    readLegacyDouble(values, "waveform_gain", waveform, "gain");
    readLegacyDouble(values, "waveform_stride", waveform, "stride");
    readLegacyDouble(values, "waveform_smoothing_ms", waveform, "smoothing_ms");
    readLegacyDouble(values, "histogram_stride", histogram, "stride");

    // matrix: 0 was BT.601 (choice 0); any other value was BT.709 (choice 1).
    if (const auto found = values.find("matrix"); found != values.end()) {
        vectorscope["matrix"] = std::strtol(found->second.c_str(), nullptr, 10) == 0 ? 0.0 : 1.0;
    }
    // trace_response: 1 was Linear (choice 1); any other value was Boosted (0).
    if (const auto found = values.find("trace_response"); found != values.end()) {
        vectorscope["response"] = std::strtol(found->second.c_str(), nullptr, 10) == 1 ? 1.0 : 0.0;
    }
    if (const auto found = values.find("waveform_mode"); found != values.end()) {
        waveform["mode"] = waveformModeChoice(std::strtol(found->second.c_str(), nullptr, 10));
    }
    // histogram_per_channel inverts into the style choice: per-channel was the
    // default and is choice 0, combined is choice 1.
    if (const auto found = values.find("histogram_per_channel"); found != values.end()) {
        histogram["style"] = found->second == "1" ? 0.0 : 1.0;
    }
}

// Reads every generic scopeId.paramKey key into the map, superseding the legacy
// values. Ids are reverse-DNS, so a key opening with the org. prefix splits at
// its last dot; this is how a third-party or letterless scope persists params
// the host never names.
void readGenericScopeParams(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    for (const auto& [key, value] : values) {
        if (key.rfind("org.", 0) != 0) {
            continue;
        }
        const auto dot = key.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= key.size()) {
            continue;
        }
        preferences.scopeParams[key.substr(0, dot)][key.substr(dot + 1)] = std::strtod(value.c_str(), nullptr);
    }
}

// The parade owns no persisted gain or stride: it mirrors the waveform. Seed
// its slots from whatever the waveform resolved to, unless a generic parade key
// already set them.
void seedParadeFromWaveform(Preferences& preferences)
{
    const auto waveform = preferences.scopeParams.find(WaveformId);
    if (waveform == preferences.scopeParams.end()) {
        return;
    }
    std::map<std::string, double>& parade = preferences.scopeParams[ParadeId];
    for (const char* key : {"gain", "stride"}) {
        if (parade.find(key) == parade.end()) {
            if (const auto source = waveform->second.find(key); source != waveform->second.end()) {
                parade[key] = source->second;
            }
        }
    }
}

// Per-scope pane weights serialize on one line as id:weight pairs joined by
// commas; scope ids never carry a colon or a comma, so the split is
// unambiguous.
std::string encodeWeights(const std::map<std::string, double>& weights)
{
    std::ostringstream out;
    bool first = true;
    for (const auto& [id, weight] : weights) {
        if (!first) {
            out << ',';
        }
        out << id << ':' << weight;
        first = false;
    }

    return out.str();
}

// Parses the id:weight,id:weight form back into a map, dropping malformed pairs
// and any non-positive weight.
std::map<std::string, double> decodeWeights(const std::string& encoded)
{
    std::map<std::string, double> weights;
    std::size_t at = 0;
    while (at < encoded.size()) {
        const auto comma = encoded.find(',', at);
        const std::string pair = encoded.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (const auto colon = pair.find(':'); colon != std::string::npos && colon > 0) {
            const double weight = std::strtod(pair.c_str() + colon + 1, nullptr);
            if (weight > 0.0) {
                weights[pair.substr(0, colon)] = weight;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        at = comma + 1;
    }

    return weights;
}

// A preset's style choices serialize on one line as scopeId.key:value pairs
// joined by commas - the same scopeId.paramKey scheme scopeParams uses. Keys
// are C identifiers without dots, so the last dot splits id from key.
std::string encodeStyles(const std::map<std::string, std::map<std::string, double>>& styles)
{
    std::ostringstream out;
    bool first = true;
    for (const auto& [id, params] : styles) {
        for (const auto& [key, value] : params) {
            if (!first) {
                out << ',';
            }
            out << id << '.' << key << ':' << value;
            first = false;
        }
    }

    return out.str();
}

// Parses the scopeId.key:value form back into the nested map, dropping any
// pair missing its colon, its dot, or either name.
std::map<std::string, std::map<std::string, double>> decodeStyles(const std::string& encoded)
{
    std::map<std::string, std::map<std::string, double>> styles;
    std::size_t at = 0;
    while (at < encoded.size()) {
        const auto comma = encoded.find(',', at);
        const std::string pair = encoded.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (const auto colon = pair.find(':'); colon != std::string::npos && colon > 0) {
            const std::string name = pair.substr(0, colon);
            if (const auto dot = name.rfind('.'); dot != std::string::npos && dot > 0 && dot + 1 < name.size()) {
                styles[name.substr(0, dot)][name.substr(dot + 1)] = std::strtod(pair.c_str() + colon + 1, nullptr);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        at = comma + 1;
    }

    return styles;
}

// An orientation is 0 automatic, 1 vertical, or 2 horizontal; anything else
// falls back to automatic, so a corrupt value never wedges the layout.
int cleanedOrientation(int value)
{
    return value >= 0 && value <= 2 ? value : 0;
}

// The live layout state: the split orientation and the current pane weights.
void readLiveLayout(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    readInt(values, "layout_orientation", preferences.layoutOrientation);
    preferences.layoutOrientation = cleanedOrientation(preferences.layoutOrientation);
    if (const auto found = values.find("layout_weights"); found != values.end()) {
        preferences.layoutWeights = decodeWeights(found->second);
    }
    readInt(values, "layout_active_slot", preferences.layoutActiveSlot);
    if (preferences.layoutActiveSlot < 0 || preferences.layoutActiveSlot > LayoutPresetSlots) {
        preferences.layoutActiveSlot = 0;
    }
}

// The saved layout slots, one prefixed group each: layout.presetN.stack,
// .orientation, .weights, and .styles. An absent stack leaves the slot
// unused.
void readLayoutPresets(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    for (int slot = 0; slot < LayoutPresetSlots; ++slot) {
        const std::string prefix = "layout.preset" + std::to_string(slot + 1) + '.';
        LayoutPreset& preset = preferences.layoutPresets[static_cast<std::size_t>(slot)];
        if (const auto found = values.find(prefix + "stack"); found != values.end()) {
            preset.stack = found->second;
        }
        readInt(values, (prefix + "orientation").c_str(), preset.orientation);
        preset.orientation = cleanedOrientation(preset.orientation);
        if (const auto found = values.find(prefix + "weights"); found != values.end()) {
            preset.weights = decodeWeights(found->second);
        }
        if (const auto found = values.find(prefix + "styles"); found != values.end()) {
            preset.styles = decodeStyles(found->second);
        }
    }
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

// Cleans a stack token string, each token once, in order, defaulting to the
// vectorscope. A bracketed `[id]` token passes through untouched for the
// registry to resolve; a bare letter is kept only when it names a built-in.
// The retired L (a separate luma waveform) becomes the waveform in its Luma
// style, unless an RGB waveform is already stacked, which keeps the letter.
std::string cleanedScopeStack(const std::string& stack, Preferences& preferences)
{
    const bool hadWaveform = stack.find('W') != std::string::npos;
    std::string cleaned;
    for (std::size_t at = 0; at < stack.size();) {
        std::string token;
        if (stack[at] == '[') {
            const auto close = stack.find(']', at);
            if (close == std::string::npos) {
                break;
            }
            token = stack.substr(at, close - at + 1);
            at = close + 1;
        } else {
            char letter = stack[at];
            ++at;
            if (letter == 'L') {
                if (hadWaveform) {
                    continue;
                }
                letter = 'W';
                preferences.scopeParams[WaveformId]["mode"] = 1.0;
            }
            if (std::string_view("VWRHC").find(letter) == std::string_view::npos) {
                continue;
            }
            token = std::string(1, letter);
        }
        if (cleaned.find(token) == std::string::npos) {
            cleaned += token;
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

// Writes the live layout state and every used preset slot. Unused slots (an
// empty stack) write nothing, so the file stays terse.
void writeLayout(std::ostream& out, const Preferences& preferences)
{
    out << "layout_orientation=" << preferences.layoutOrientation << '\n'
        << "layout_weights=" << encodeWeights(preferences.layoutWeights) << '\n'
        << "layout_active_slot=" << preferences.layoutActiveSlot << '\n';
    for (int slot = 0; slot < LayoutPresetSlots; ++slot) {
        const LayoutPreset& preset = preferences.layoutPresets[static_cast<std::size_t>(slot)];
        if (preset.stack.empty()) {
            continue;
        }
        const std::string prefix = "layout.preset" + std::to_string(slot + 1) + '.';
        out << prefix << "stack=" << preset.stack << '\n'
            << prefix << "orientation=" << preset.orientation << '\n'
            << prefix << "weights=" << encodeWeights(preset.weights) << '\n';
        if (!preset.styles.empty()) {
            out << prefix << "styles=" << encodeStyles(preset.styles) << '\n';
        }
    }
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
    readLegacyScopeParams(values, preferences);
    readGenericScopeParams(values, preferences);
    seedParadeFromWaveform(preferences);

    // The retired waveform_mode value also selects the legacy stack letters, so
    // it is read once here and handed to the migration.
    int storedWaveformMode = static_cast<int>(WaveformMode::Rgb);
    readInt(values, "waveform_mode", storedWaveformMode);
    migrateScopeStack(values, storedWaveformMode, preferences);

    readBool(values, "show_graticule", preferences.showGraticule);
    readInt(values, "vectorscope_zoom", preferences.vectorscopeZoom);
    if (preferences.vectorscopeZoom != 2 && preferences.vectorscopeZoom != 4) {
        preferences.vectorscopeZoom = 1;
    }

    readLiveLayout(values, preferences);
    readLayoutPresets(values, preferences);

    readShortcuts(values, preferences.shortcuts);
    readScopeShortcuts(values, preferences);

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
    // Generic scope parameters: scopeId.paramKey=value. The parade is never
    // written; it mirrors the waveform and re-seeds from it on load.
    for (const auto& [id, params] : preferences.scopeParams) {
        if (id == ParadeId) {
            continue;
        }
        for (const auto& [key, value] : params) {
            out << id << '.' << key << '=' << value << '\n';
        }
    }
    out << "scope_stack=" << preferences.scopeStack << '\n'
        << "show_graticule=" << (preferences.showGraticule ? 1 : 0) << '\n'
        << "vectorscope_zoom=" << preferences.vectorscopeZoom << '\n'
        << "window_x=" << preferences.windowX << '\n'
        << "window_y=" << preferences.windowY << '\n'
        << "window_width=" << preferences.windowWidth << '\n'
        << "window_height=" << preferences.windowHeight << '\n'
        << "shortcut_attach_window=" << preferences.shortcuts.attachWindow << '\n'
        << "shortcut_draw_area=" << preferences.shortcuts.drawArea << '\n'
        << "shortcut_select_face=" << preferences.shortcuts.selectFace << '\n'
        << "shortcut_pin_color=" << preferences.shortcuts.pinColor << '\n'
        << "shortcut_vectorscope_zoom=" << preferences.shortcuts.vectorscopeZoom << '\n'
        << "shortcut_full_screen=" << preferences.shortcuts.fullScreen << '\n';
    // Scope-toggle bindings keyed by scope id: only overrides are written, each
    // as shortcut_<id>, so a scope at its default letter needs no line.
    for (const auto& [id, letter] : preferences.scopeShortcuts) {
        out << "shortcut_" << id << '=' << letter << '\n';
    }
    writeLayout(out, preferences);

    std::ofstream output(file, std::ios::trunc);
    if (!output) {
        return false;
    }
    output << out.str();
    return static_cast<bool>(output);
}

}  // namespace sidescopes
