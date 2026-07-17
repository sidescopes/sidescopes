#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace sidescopes {

/// Keyboard bindings for every shortcut the application offers, editable
/// in the preferences file. Each is a single letter A-Z or "Escape";
/// anything else falls back to the default. Shift composes where an
/// action documents it (stacking scopes, pinning the region average).
struct ShortcutBindings
{
    std::string vectorscope = "V";
    std::string waveform = "W";
    std::string parade = "R";
    std::string histogram = "H";
    std::string colorPicker = "C";
    std::string pickWindow = "A";
    std::string drawRegion = "D";
    std::string pickFaces = "F";
    std::string pinColor = "P";
    std::string vectorscopeZoom = "Z";
    std::string fullRegion = "Escape";
};

/// Everything worth remembering between sessions. Serialized as a small
/// key=value text file: trivially diffable, no dependency, and unknown keys
/// are ignored so older builds tolerate newer files.
///
/// Every scope's parameters live under a generic `scopeId.paramKey` scheme in
/// @ref scopeParams, so a scope persists its settings without the host naming
/// each one. Values are the module descriptors' own numbers (gains verbatim,
/// choice parameters as their index); the host smoothing controls ride the
/// same map under the `smoothing_ms` key.
struct Preferences
{
    /// Scope id to parameter key to value. Seeded to the built-in defaults so a
    /// missing file or key yields the shipped live state. The parade is not
    /// held here on save: it mirrors the waveform, and is re-seeded from it on
    /// load.
    std::map<std::string, std::map<std::string, double>> scopeParams{
        {"org.sidescopes.vectorscope",
         {{"gain", 3.0}, {"stride", 1.0}, {"matrix", 1.0}, {"response", 0.0}, {"smoothing_ms", 75.0}}},
        {"org.sidescopes.waveform", {{"gain", 0.05}, {"stride", 1.0}, {"mode", 0.0}, {"smoothing_ms", 100.0}}},
        {"org.sidescopes.histogram", {{"stride", 1.0}, {"style", 0.0}}},
    };
    /// The scopes on screen, one letter each in stacking order: V
    /// vectorscope, W waveform, R RGB parade, H histogram, C color picker.
    /// Never empty. The retired L (a separate luma waveform) is accepted
    /// only as a legacy migration input, folded into W in its Luma style.
    std::string scopeStack = "V";
    bool showGraticule = true;
    /// Magnify-view factor for the vectorscope: 1, 2, or 4.
    int vectorscopeZoom = 1;
    int windowX = -1;  ///< Negative lets the system place the window.
    int windowY = -1;
    int windowWidth = 440;
    int windowHeight = 500;
    ShortcutBindings shortcuts;
};

/// Missing or unreadable files yield the defaults; malformed lines and
/// unknown keys are skipped.
[[nodiscard]] Preferences loadPreferences(const std::filesystem::path& file);

/// Creates parent directories as needed. Returns false when writing failed.
[[nodiscard]] bool savePreferences(const Preferences& preferences, const std::filesystem::path& file);

}  // namespace sidescopes
