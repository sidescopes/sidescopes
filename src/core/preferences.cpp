#include "core/preferences.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/environment.h"

namespace sidescopes {
namespace {

constexpr char WaveformId[] = "org.sidescopes.waveform";
constexpr char ParadeId[] = "org.sidescopes.parade";

std::ostringstream numericStream()
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(std::numeric_limits<double>::max_digits10);
    return out;
}

template <typename Number>
std::optional<Number> parseNumber(std::string_view text)
{
    if (text.empty() || text.front() == '+') {
        return std::nullopt;
    }
    Number value{};
    std::istringstream input{std::string{text}};
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    if (!input || !input.eof() || !std::isfinite(value)) {
        return std::nullopt;
    }

    return value;
}

std::map<std::string, std::string, std::less<>> parseKeyValueLines(std::istream& input)
{
    std::map<std::string, std::string, std::less<>> values;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
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
        if (const auto value = parseNumber<int>(found->second)) {
            out = *value;
        }
    }
}

void readFloat(const std::map<std::string, std::string, std::less<>>& values, const char* key, float& out)
{
    if (const auto found = values.find(key); found != values.end()) {
        if (const auto value = parseNumber<float>(found->second)) {
            out = *value;
        }
    }
}

void readBool(const std::map<std::string, std::string, std::less<>>& values, const char* key, bool& out)
{
    if (const auto found = values.find(key); found != values.end()) {
        if (found->second == "0") {
            out = false;
        } else if (found->second == "1") {
            out = true;
        }
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
    readShortcut(values, "shortcut_draw_region", shortcuts.drawRegion);
    readShortcut(values, "shortcut_attach_face", shortcuts.attachFace);
    readShortcut(values, "shortcut_pin_color", shortcuts.pinColor);
    readShortcut(values, "shortcut_vectorscope_zoom", shortcuts.vectorscopeZoom);
    readShortcut(values, "shortcut_clear_region", shortcuts.clearRegion);
}

// Reverse-DNS ids have nonempty dot-separated components. The layout and
// shortcut namespaces belong to the file format, not to module parameters.
bool validScopeId(std::string_view id)
{
    if (id.empty() || id.starts_with("layout.") || id.starts_with("shortcut_") ||
        id.find('.') == std::string_view::npos || id.front() == '.' || id.back() == '.' ||
        id.find("..") != std::string_view::npos) {
        return false;
    }
    return id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
           std::string_view::npos;
}

// Scope shortcuts use the same ids as the module registry.
void readScopeShortcuts(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    constexpr std::string_view KeyPrefix = "shortcut_";
    for (const auto& [key, value] : values) {
        if (key.starts_with(KeyPrefix) && validScopeId(std::string_view(key).substr(KeyPrefix.size())) &&
            validBinding(value)) {
            preferences.scopeShortcuts[key.substr(KeyPrefix.size())] = value;
        }
    }
}

// Reads each numeric scopeId.paramKey into the map. The last dot separates
// the reverse-DNS id from the parameter, regardless of the scope's domain.
void readGenericScopeParams(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    for (const auto& [key, value] : values) {
        const auto dot = key.rfind('.');
        if (dot == std::string::npos || dot + 1 >= key.size() || !validScopeId(std::string_view(key).substr(0, dot))) {
            continue;
        }
        if (const auto number = parseNumber<double>(value)) {
            preferences.scopeParams[key.substr(0, dot)][key.substr(dot + 1)] = *number;
        }
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
    auto out = numericStream();
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
            const auto weight = parseNumber<double>(std::string_view(pair).substr(colon + 1));
            if (weight && *weight > 0.0 && *weight <= std::numeric_limits<float>::max() &&
                static_cast<float>(*weight) > 0.0f) {
                weights[pair.substr(0, colon)] = *weight;
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
    auto out = numericStream();
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
                if (const auto value = parseNumber<double>(std::string_view(pair).substr(colon + 1))) {
                    styles[name.substr(0, dot)][name.substr(dot + 1)] = *value;
                }
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        at = comma + 1;
    }

    return styles;
}

// One channel of a pinned color as the two hex digits the file stores. Samples
// ride on the 0..255 scale in floating point, and averaging a swatch lands
// between two codes, so the value is rounded and clamped rather than truncated.
void appendPinChannel(std::string& out, float value)
{
    constexpr char HexDigits[] = "0123456789ABCDEF";
    const int code = static_cast<int>(std::lround(std::clamp(value, 0.0f, 255.0f)));
    out += HexDigits[code >> 4];
    out += HexDigits[code & 0xF];
}

// The pinned colors serialize on one line as RRGGBB hex joined by commas: the
// notation the picker itself shows, and exact for 8-bit samples.
std::string encodePins(const std::vector<FloatColor>& pins)
{
    std::string encoded;
    for (const FloatColor& pin : pins) {
        if (!encoded.empty()) {
            encoded += ',';
        }
        appendPinChannel(encoded, pin.r);
        appendPinChannel(encoded, pin.g);
        appendPinChannel(encoded, pin.b);
    }

    return encoded;
}

// One RRGGBB token as a color, or nothing when it is not exactly six hex
// digits. A leading # is accepted, because that is the form the picker copies
// to the clipboard and the form a user pastes back.
std::optional<FloatColor> decodeHexColor(const std::string& token)
{
    const std::string digits = !token.empty() && token.front() == '#' ? token.substr(1) : token;
    if (digits.size() != 6 || digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return std::nullopt;
    }
    const long packed = std::strtol(digits.c_str(), nullptr, 16);

    return FloatColor{static_cast<float>((packed >> 16) & 0xFF), static_cast<float>((packed >> 8) & 0xFF),
                      static_cast<float>(packed & 0xFF)};
}

// Parses the RRGGBB,RRGGBB form back into colors, skipping any malformed token
// and stopping at MaximumPins, so a hand-edited file can neither corrupt a pin
// nor overflow the ring.
std::vector<FloatColor> decodePins(const std::string& encoded)
{
    std::vector<FloatColor> pins;
    std::size_t at = 0;
    while (at < encoded.size() && pins.size() < MaximumPins) {
        const auto comma = encoded.find(',', at);
        const std::string token = encoded.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (const auto color = decodeHexColor(token)) {
            pins.push_back(*color);
        }
        if (comma == std::string::npos) {
            break;
        }
        at = comma + 1;
    }

    return pins;
}

// The comparison reference, read strictly: only a plain index into the colors
// the file carried selects a pin. Junk, the file's own -1, and an index past
// the list alike leave no reference, never a pin the user did not choose.
int decodePinComparator(const std::string& value, std::size_t pinCount)
{
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
        return -1;
    }
    const long index = std::strtol(value.c_str(), nullptr, 10);

    return index < static_cast<long>(pinCount) ? static_cast<int>(index) : -1;
}

// The pinned colors and the reference selected among them. A session that
// pinned nothing writes neither key, and its file loads an empty board.
void readPins(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    if (const auto found = values.find("pins"); found != values.end()) {
        preferences.pins = decodePins(found->second);
    }
    if (const auto found = values.find("pin_comparator"); found != values.end()) {
        preferences.pinComparator = decodePinComparator(found->second, preferences.pins.size());
    }
}

// Takes the spaces off both ends of @p text, leaving nothing when it is all
// spaces.
void trimSpaces(std::string& text)
{
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        text.clear();

        return;
    }
    text = text.substr(first, text.find_last_not_of(' ') - first + 1);
}

// Cuts @p text to at most MaximumPresetNameLength bytes without splitting a
// UTF-8 sequence: a cut landing inside one backs off to where the character
// began, dropping it whole rather than leaving a byte no font can draw.
void truncateOnCharacter(std::string& text)
{
    if (text.size() <= MaximumPresetNameLength) {
        return;
    }
    std::size_t at = MaximumPresetNameLength;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
        --at;
    }
    text.resize(at);
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
    // There is no "no preset" any more, so a file naming one - a hand edit, or
    // one written before the application was always on a preset - opens on the
    // first slot rather than on nothing.
    readInt(values, "layout_active_slot", preferences.layoutActiveSlot);
    if (preferences.layoutActiveSlot < 1 || preferences.layoutActiveSlot > LayoutPresetSlots) {
        preferences.layoutActiveSlot = 1;
    }
}

// The saved layout slots, one prefixed group each: layout.presetN.stack,
// .name, .orientation, .weights, and .styles. An absent stack leaves the slot
// unused, and an absent name leaves it on its default one.
void readLayoutPresets(const std::map<std::string, std::string, std::less<>>& values, Preferences& preferences)
{
    for (int slot = 0; slot < LayoutPresetSlots; ++slot) {
        const std::string prefix = "layout.preset" + std::to_string(slot + 1) + '.';
        LayoutPreset& preset = preferences.layoutPresets[static_cast<std::size_t>(slot)];
        if (const auto found = values.find(prefix + "stack"); found != values.end()) {
            preset.stack = found->second;
        }
        if (const auto found = values.find(prefix + "order"); found != values.end()) {
            preset.order = found->second;
        }
        if (const auto found = values.find(prefix + "name"); found != values.end()) {
            preset.name = sanitizedPresetName(found->second);
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

// A stack as the file may carry it: bracketed ids, duplicates dropped, and
// anything else ignored. Core does not know the scope set, so it never judges
// WHICH scopes the ids name - only the token format - and leaves the registry
// to resolve them.
std::string cleanedScopeStack(const std::string& stack)
{
    std::string cleaned;
    for (std::size_t at = stack.find('['); at != std::string::npos; at = stack.find('[', at)) {
        const auto close = stack.find(']', at);
        if (close == std::string::npos) {
            break;
        }
        const std::string token = stack.substr(at, close - at + 1);
        at = close + 1;
        if (cleaned.find(token) == std::string::npos) {
            cleaned += token;
        }
    }

    return cleaned;
}

// Writes the live layout state and every preset slot worth a line. An unused
// slot writes nothing beyond a name it has been given, so the file stays terse
// without losing a name the user chose before filling the slot.
void writeLayout(std::ostream& out, const Preferences& preferences)
{
    out << "layout_orientation=" << preferences.layoutOrientation << '\n'
        << "layout_weights=" << encodeWeights(preferences.layoutWeights) << '\n'
        << "layout_active_slot=" << preferences.layoutActiveSlot << '\n';
    for (int slot = 0; slot < LayoutPresetSlots; ++slot) {
        const LayoutPreset& preset = preferences.layoutPresets[static_cast<std::size_t>(slot)];
        const std::string prefix = "layout.preset" + std::to_string(slot + 1) + '.';
        // Sanitized rather than trusted: a name carrying a newline would end
        // the line early and the rest of it would read back as a key.
        if (!preset.name.empty()) {
            out << prefix << "name=" << sanitizedPresetName(preset.name) << '\n';
        }
        if (preset.stack.empty()) {
            continue;
        }
        out << prefix << "stack=" << preset.stack << '\n'
            << prefix << "order=" << preset.order << '\n'
            << prefix << "orientation=" << preset.orientation << '\n'
            << prefix << "weights=" << encodeWeights(preset.weights) << '\n';
        if (!preset.styles.empty()) {
            out << prefix << "styles=" << encodeStyles(preset.styles) << '\n';
        }
    }
}

// Reserving a sibling directory is exclusive across processes and keeps the
// pending file on the destination's filesystem, where replacement is atomic.
// The previous file is untouched until the complete new file closes cleanly.
bool replacePreferences(const std::filesystem::path& file, const std::string& contents)
{
    std::error_code error;
    if (!file.parent_path().empty()) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (error) {
            return false;
        }
    }

    static std::atomic<unsigned long long> sequence{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path directory;
    for (int attempt = 0; attempt < 64; ++attempt) {
        directory = file.parent_path() / ".sidescopes-preferences-";
        directory += std::to_string(stamp) + '-' + std::to_string(sequence.fetch_add(1));
        if (std::filesystem::create_directory(directory, error)) {
            break;
        }
        if (error || attempt == 63) {
            return false;
        }
    }

    struct Cleanup
    {
        const std::filesystem::path& directory;

        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    } cleanup{directory};

    const std::filesystem::path pending = directory / "preferences";
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << contents;
    output.close();
    if (!output) {
        return false;
    }
    std::filesystem::rename(pending, file, error);
    return !error;
}

}  // namespace

std::string sanitizedPresetName(std::string_view name)
{
    std::string cleaned;
    for (const char character : name) {
        // Everything below a space, and the delete code, would either end the
        // line the file stores this on or draw as nothing.
        const auto code = static_cast<unsigned char>(character);
        if (code >= 0x20 && code != 0x7F) {
            cleaned += character;
        }
    }
    // Trimmed before the cut, so the limit bounds the name the user sees
    // rather than the whitespace they happened to type around it, and again
    // after, because the cut itself can leave a trailing space.
    trimSpaces(cleaned);
    truncateOnCharacter(cleaned);
    trimSpaces(cleaned);

    return cleaned;
}

Preferences loadPreferences(const std::filesystem::path& file)
{
    Preferences preferences;
    std::ifstream input(file);
    if (!input) {
        return preferences;
    }

    const auto values = parseKeyValueLines(input);
    readGenericScopeParams(values, preferences);
    seedParadeFromWaveform(preferences);

    if (const auto found = values.find("scope_stack"); found != values.end()) {
        preferences.scopeStack = cleanedScopeStack(found->second);
    }
    if (const auto found = values.find("scope_order"); found != values.end()) {
        preferences.scopeOrder = cleanedScopeStack(found->second);
    }

    readFloat(values, "graticule_strength", preferences.graticuleStrength);
    readInt(values, "vectorscope_zoom", preferences.vectorscopeZoom);
    readBool(values, "show_cursor_markers", preferences.showCursorMarkers);
    readInt(values, "tour_settled", preferences.tourSettled);
    if (preferences.vectorscopeZoom != 2 && preferences.vectorscopeZoom != 4) {
        preferences.vectorscopeZoom = 1;
    }

    readLiveLayout(values, preferences);
    readLayoutPresets(values, preferences);

    readShortcuts(values, preferences.shortcuts);
    readScopeShortcuts(values, preferences);
    readPins(values, preferences);

    readFloat(values, "ui_scale_factor", preferences.uiScaleFactor);
    const auto quality = values.find("quality");
    if (quality != values.end()) {
        preferences.quality = quality->second;
    }
    const auto x = values.find("window_x");
    const auto y = values.find("window_y");
    if (x != values.end() && y != values.end()) {
        const auto positionX = parseNumber<int>(x->second);
        const auto positionY = parseNumber<int>(y->second);
        if (positionX && positionY) {
            preferences.windowPosition = WindowPosition{*positionX, *positionY};
        }
    }
    readInt(values, "window_width", preferences.windowWidth);
    readInt(values, "window_height", preferences.windowHeight);

    if (preferences.windowWidth <= 0) {
        preferences.windowWidth = Preferences{}.windowWidth;
    }
    if (preferences.windowHeight <= 0) {
        preferences.windowHeight = Preferences{}.windowHeight;
    }

    return preferences;
}

std::string preferencesFileFromEnvironment()
{
    return environmentValue(PreferencesFileVariable);
}

bool savePreferences(const Preferences& preferences, const std::filesystem::path& file)
{
    auto out = numericStream();
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
        << "scope_order=" << preferences.scopeOrder << '\n'
        << "graticule_strength=" << preferences.graticuleStrength << '\n'
        << "vectorscope_zoom=" << preferences.vectorscopeZoom << '\n'
        << "show_cursor_markers=" << (preferences.showCursorMarkers ? 1 : 0) << '\n'
        << "tour_settled=" << preferences.tourSettled << '\n'
        << "ui_scale_factor=" << preferences.uiScaleFactor << '\n'
        << "quality=" << preferences.quality << '\n'
        << "window_width=" << preferences.windowWidth << '\n'
        << "window_height=" << preferences.windowHeight << '\n'
        << "shortcut_attach_window=" << preferences.shortcuts.attachWindow << '\n'
        << "shortcut_draw_region=" << preferences.shortcuts.drawRegion << '\n'
        << "shortcut_attach_face=" << preferences.shortcuts.attachFace << '\n'
        << "shortcut_pin_color=" << preferences.shortcuts.pinColor << '\n'
        << "shortcut_vectorscope_zoom=" << preferences.shortcuts.vectorscopeZoom << '\n'
        << "shortcut_clear_region=" << preferences.shortcuts.clearRegion << '\n';
    if (preferences.windowPosition) {
        out << "window_x=" << preferences.windowPosition->x << '\n'
            << "window_y=" << preferences.windowPosition->y << '\n';
    }
    // Scope-toggle bindings keyed by scope id: only overrides are written, each
    // as shortcut_<id>, so a scope at its default letter needs no line.
    for (const auto& [id, letter] : preferences.scopeShortcuts) {
        out << "shortcut_" << id << '=' << letter << '\n';
    }
    // An empty board writes neither pin key, so the file stays terse.
    if (!preferences.pins.empty()) {
        out << "pins=" << encodePins(preferences.pins) << '\n'
            << "pin_comparator=" << preferences.pinComparator << '\n';
    }
    writeLayout(out, preferences);

    return out && replacePreferences(file, out.str());
}

}  // namespace sidescopes
