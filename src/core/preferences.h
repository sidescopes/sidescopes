#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace sidescopes {

/// Keyboard bindings for the non-scope actions the application offers, editable
/// in the preferences file. Each is a single letter A-Z or "Escape"; anything
/// else falls back to the default. Shift composes where an action documents it
/// (pinning the region average). Scope-toggle bindings are keyed by scope id in
/// @ref Preferences::scopeShortcuts, not here.
struct ShortcutBindings
{
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
    /// The scopes on screen in stacking order. New files store one token per
    /// scope: a bracketed `[id]` for a letterless scope, otherwise the scope's
    /// letter. Legacy files store bare letters (V vectorscope, W waveform, R
    /// RGB parade, H histogram, C color picker); the retired L (a separate luma
    /// waveform) is accepted only as a migration input, folded into W in its
    /// Luma style. Never empty.
    std::string scopeStack = "V";
    bool showGraticule = true;
    /// Magnify-view factor for the vectorscope: 1, 2, or 4.
    int vectorscopeZoom = 1;
    /// Live layout split orientation: 0 automatic (the historical longer-axis
    /// split), 1 vertical (stacked), 2 horizontal (side by side).
    int layoutOrientation = 0;
    /// Live per-scope pane weights, keyed by scope id; a scope absent from the
    /// map weighs 1. Equal weights reproduce the original even split.
    std::map<std::string, double> layoutWeights;
    int windowX = -1;  ///< Negative lets the system place the window.
    int windowY = -1;
    int windowWidth = 440;
    int windowHeight = 500;
    ShortcutBindings shortcuts;
    /// Per-scope toggle bindings, keyed by scope id, holding only the overrides
    /// a file names; every other scope defaults to its descriptor letter, which
    /// the host supplies. A custom binding is a file edit away without a host UI
    /// that assigns one.
    std::map<std::string, std::string> scopeShortcuts;
};

/// Missing or unreadable files yield the defaults; malformed lines and
/// unknown keys are skipped.
[[nodiscard]] Preferences loadPreferences(const std::filesystem::path& file);

/// Creates parent directories as needed. Returns false when writing failed.
[[nodiscard]] bool savePreferences(const Preferences& preferences, const std::filesystem::path& file);

}  // namespace sidescopes
