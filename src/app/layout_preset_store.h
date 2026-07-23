#pragma once

#include <array>

#include "core/preferences.h"

namespace sidescopes {

/// Owns the layout preset slots and which one is active. Capturing a preset
/// from the live view and applying one back to it stay with the host (they are
/// view I/O); this holds the stored slots and answers whether the live layout
/// has drifted from the active one.
class LayoutPresetStore
{
public:
    /// @return The preset in @p slot (1-based). Every slot is present, empty
    ///         until saved.
    [[nodiscard]] const LayoutPreset& at(int slot) const;

    /// Stores @p preset in @p slot (1-based) and makes it the active slot.
    void save(int slot, LayoutPreset preset);

    /// Records @p slot (1-based) as active after a load.
    void markLoaded(int slot);

    /// @return The active slot (1-based), or 0 when none is active.
    [[nodiscard]] int activeSlot() const;

    /// @return Whether @p live differs from the active slot's stored preset;
    ///         false when no slot is active.
    [[nodiscard]] bool isDirty(const LayoutPreset& live) const;

    /// @return All slots, for the picker to list and for persistence.
    [[nodiscard]] const std::array<LayoutPreset, LayoutPresetSlots>& all() const;

    /// Replaces the slots and the active slot, as loaded from preferences.
    void restore(const std::array<LayoutPreset, LayoutPresetSlots>& presets, int activeSlot);

private:
    std::array<LayoutPreset, LayoutPresetSlots> m_presets{};
    int m_activeSlot = 0;
};

}  // namespace sidescopes
