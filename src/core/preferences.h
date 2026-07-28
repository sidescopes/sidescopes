#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/frame.h"

namespace sidescopes {

/// How many quick-access layout preset slots the app offers, bound to the
/// number keys 1-9.
inline constexpr int LayoutPresetSlots = 9;

/// How many pinned colors a file may carry. This restates PinBoard::Maximum,
/// the pin ring's capacity: the class lives in the application layer, which
/// the core cannot see, so a hand-edited file is capped here instead. The two
/// constants are asserted equal where the board is seeded.
inline constexpr std::size_t MaximumPins = 8;

/// How long a preset's name may be, in bytes. The menus size themselves to
/// their widest entry, so a name is bounded to keep a list the width of a
/// list.
inline constexpr std::size_t MaximumPresetNameLength = 24;

/// One saved layout slot: the scope stack, its split orientation, the
/// per-scope pane weights, and what the user calls it. An empty @ref stack
/// marks an unused slot, which loading treats as a no-op. Weights are keyed by
/// scope id; a scope absent from the map keeps the default weight of 1.
struct LayoutPreset
{
    std::string stack;  ///< Stack tokens, the scopeStack format; empty = unused.
    /// What the user calls this slot. Empty means it has never been renamed,
    /// and the application shows the slot's default name; a name is kept even
    /// for a slot holding no layout, so a set of slots can be named before
    /// they are filled.
    std::string name;
    int orientation = 0;                    ///< 0 automatic, 1 vertical, 2 horizontal.
    std::map<std::string, double> weights;  ///< Scope id to pane weight.
    /// Choice-parameter values - the right-click style menus' state - per
    /// scope id, captured when the preset is saved. Scopes and keys absent
    /// here keep their live values on load; continuous parameters (gain,
    /// smoothing) never enter a preset.
    std::map<std::string, std::map<std::string, double>> styles;
};

/// Keyboard bindings for the non-scope actions the application offers, editable
/// in the preferences file. Each is a single letter A-Z or "Escape"; anything
/// else falls back to the default. Shift composes where an action documents it
/// (pinning the region average). Scope-toggle bindings are keyed by scope id in
/// @ref Preferences::scopeShortcuts, not here.
struct ShortcutBindings
{
    std::string attachWindow = "A";
    std::string drawRegion = "D";
    std::string attachFace = "F";
    std::string pinColor = "P";
    std::string vectorscopeZoom = "Z";
    std::string clearRegion = "Escape";
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
        {"org.sidescopes.vectorscope", {{"gain", 3.0}, {"stride", 1.0}, {"response", 0.0}, {"smoothing_ms", 75.0}}},
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
    /// The order the user keeps the scopes in - the sequence the menu lists
    /// them in and the panes follow - in the same token vocabulary as
    /// @ref scopeStack, and naming scopes whether or not they are on screen.
    /// Empty means no preference, and the application falls back to its
    /// registration order; scopes the string leaves out follow the ones it
    /// names. The application is the only judge of which tokens are real, so
    /// the value is stored as read.
    std::string scopeOrder;
    /// How strongly the graticule is drawn, a multiplier on its shipped ink and
    /// one of the discrete GraticuleStrengths. Bounded below rather than
    /// switchable off, so a stored value can quieten the graticule but never
    /// take it away.
    float graticuleStrength = 1.0f;
    /// Magnify-view factor for the vectorscope: 1, 2, or 4.
    int vectorscopeZoom = 1;
    /// Live layout split orientation: 0 automatic (the historical longer-axis
    /// split), 1 vertical (stacked), 2 horizontal (side by side).
    int layoutOrientation = 0;
    /// Live per-scope pane weights, keyed by scope id; a scope absent from the
    /// map weighs 1. Equal weights reproduce the original even split.
    std::map<std::string, double> layoutWeights;
    /// Saved layout slots 1-9 (index 0 is slot 1); an empty stack marks unused.
    std::array<LayoutPreset, LayoutPresetSlots> layoutPresets;
    /// The last loaded or saved preset slot, 1-9; 0 when no preset is active.
    /// Drives the toolbar's preset badge across sessions.
    int layoutActiveSlot = 0;
    /// User interface-size factor, a multiplier on the OS scale (1.0 = match
    /// the system). One of the discrete UiScaleSteps; per user, not per display.
    float uiScaleFactor = 1.0f;
    /// How much of the machine the analysis may spend, as the level's own word:
    /// "standard" or "high". The app resolves it; anything else reads as the
    /// default.
    std::string quality = "standard";
    int windowX = -1;  ///< Negative lets the system place the window.
    int windowY = -1;
    int windowWidth = 440;
    int windowHeight = 500;
    ShortcutBindings shortcuts;
    /// The pinned reference colors, oldest first, at most @ref MaximumPins.
    /// They serialize as hex, so a color reloads at the 8 bits the picker
    /// shows; an averaged swatch loses its fraction of a code value.
    std::vector<FloatColor> pins;
    /// Which pinned color is the comparison reference, as an index into
    /// @ref pins; -1 for none.
    int pinComparator = -1;
    /// Per-scope toggle bindings, keyed by scope id, holding only the overrides
    /// a file names; every other scope defaults to its descriptor letter, which
    /// the host supplies. A custom binding is a file edit away without a host UI
    /// that assigns one.
    std::map<std::string, std::string> scopeShortcuts;
};

/// @return @p name as a preset may carry it: control characters dropped, the
///         ends trimmed of spaces, and at most @ref MaximumPresetNameLength
///         bytes, cut on a character boundary so a multi-byte character is
///         never left half written. A newline would end the line the file
///         stores the name on, so this runs on the way in from the user and
///         again on the way in from the file.
[[nodiscard]] std::string sanitizedPresetName(std::string_view name);

/// The environment variable that moves the preferences file elsewhere.
inline constexpr char PreferencesFileVariable[] = "SIDESCOPES_PREFS_FILE";

/// The preferences file the environment names, or empty for the usual one.
///
/// A throwaway instance - a measurement harness driving the application, a test
/// launching it - needs its own scope stack and window placement without
/// inheriting or overwriting the user's. Pointing this at a scratch file gives
/// it exactly that: the real file is never read and never written.
[[nodiscard]] std::string preferencesFileFromEnvironment();

/// Missing or unreadable files yield the defaults; malformed lines and
/// unknown keys are skipped.
[[nodiscard]] Preferences loadPreferences(const std::filesystem::path& file);

/// Creates parent directories as needed. Returns false when writing failed.
[[nodiscard]] bool savePreferences(const Preferences& preferences, const std::filesystem::path& file);

}  // namespace sidescopes
