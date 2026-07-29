#pragma once

#include <array>
#include <string>
#include <string_view>

#include "core/preferences.h"

namespace sidescopes {

/// @return Whether two presets hold the same layout. Names are not compared:
///         a slot's name belongs to the slot, not to what it holds.
[[nodiscard]] bool sameLayout(const LayoutPreset& first, const LayoutPreset& second);

/// @return What @p slot (1-based) is called wherever it is listed: the name
///         its user gave it, or "Preset N" until one is given. Every surface
///         asks this, so a slot is never named two ways.
[[nodiscard]] std::string presetDisplayName(int slot, const LayoutPreset& preset);

/// @return @p slot's display name wrapped in quotes, for a name that appears
///         INSIDE a sentence of ours.
///
/// A name is whatever the user typed, so dropped bare into prose it becomes
/// part of our grammar: "Whatever loaded" parses as a sentence about something
/// else entirely, and no wording of ours can prevent it because the subject is
/// theirs. The quotes say the word is a name rather than a word, which is what
/// makes a preset called "loaded", or "Preset 3 and 4", or a single letter,
/// stop being ambiguous. A name standing ALONE - a row in a list - is not in a
/// sentence and needs none of this.
[[nodiscard]] std::string quotedPresetName(int slot, const LayoutPreset& preset);

/// Owns the layout preset slots and which one is active. Capturing a preset
/// from the live view and applying one back to it stay with the host (they are
/// view I/O); this holds the stored slots and nothing else.
///
/// One slot is always active: the application is always on a preset, so there
/// is no state in which loading is meaningless and none a click can be refused
/// from.
class LayoutPresetStore
{
public:
    /// @return The preset in @p slot (1-based) as it is stored. Every slot is
    ///         present, holding nothing until saved.
    [[nodiscard]] const LayoutPreset& at(int slot) const;

    /// @return The layout @p slot (1-based) restores: what it holds, or
    ///         @p defaults while it holds nothing, under the slot's own name
    ///         either way - a slot may be named before it is filled.
    [[nodiscard]] LayoutPreset effective(int slot, const LayoutPreset& defaults) const;

    /// Stores @p preset in @p slot (1-based), keeping whatever that slot is
    /// called. Which slot is ACTIVE is not touched: a write is not a move, and
    /// the two callers want opposite things - the live layout writing itself
    /// into the slot it is already on, and a copy landing in one it is not.
    void store(int slot, LayoutPreset preset);

    /// Records @p slot (1-based) as active after a load.
    void markLoaded(int slot);

    /// Calls @p slot (1-based) @p name, cleaned the way the file will carry
    /// it. An empty or all-blank name puts the slot back on its default one.
    /// Renaming neither fills nor empties a slot, so a name can be given
    /// before there is a layout to give it to.
    void rename(int slot, std::string_view name);

    /// @return The active slot, always one of the nine.
    [[nodiscard]] int activeSlot() const;

    /// @return All slots, for the picker to list and for persistence.
    [[nodiscard]] const std::array<LayoutPreset, LayoutPresetSlots>& all() const;

    /// Replaces the slots and the active slot, as loaded from preferences. An
    /// @p activeSlot outside the nine - which is what a file written before
    /// there was always an active one carries - lands on the first.
    void restore(const std::array<LayoutPreset, LayoutPresetSlots>& presets, int activeSlot);

private:
    std::array<LayoutPreset, LayoutPresetSlots> m_presets{};
    int m_activeSlot = 1;
};

}  // namespace sidescopes
