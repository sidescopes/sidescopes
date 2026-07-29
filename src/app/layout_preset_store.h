#pragma once

#include <array>
#include <string>
#include <string_view>

#include "core/preferences.h"

namespace sidescopes {

/// @return What @p slot (1-based) is called wherever it is listed: the name
///         its user gave it, or "Preset N" until one is given. Every surface
///         asks this, so a slot is never named two ways.
[[nodiscard]] std::string presetDisplayName(int slot, const LayoutPreset& preset);

/// Owns the layout preset slots and which one is active. Capturing a preset
/// from the live view and applying one back to it stay with the host (they are
/// view I/O); this holds the stored slots and answers whether the live layout
/// has drifted from the active one.
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

    /// Stores @p preset in @p slot (1-based) and makes it the active slot.
    void save(int slot, LayoutPreset preset);

    /// Records @p slot (1-based) as active after a load.
    void markLoaded(int slot);

    /// Calls @p slot (1-based) @p name, cleaned the way the file will carry
    /// it. An empty or all-blank name puts the slot back on its default one.
    /// Renaming neither fills nor empties a slot, so a name can be given
    /// before there is a layout to give it to.
    void rename(int slot, std::string_view name);

    /// @return The active slot, always one of the nine.
    [[nodiscard]] int activeSlot() const;

    /// @return Whether @p live differs from what the active slot would
    ///         restore, @p defaults included where that slot holds nothing.
    [[nodiscard]] bool isDirty(const LayoutPreset& live, const LayoutPreset& defaults) const;

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
