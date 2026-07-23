#include "app/layout_preset_store.h"

#include <cstddef>
#include <utility>

namespace sidescopes {

const LayoutPreset& LayoutPresetStore::at(int slot) const
{
    return m_presets[static_cast<std::size_t>(slot - 1)];
}

void LayoutPresetStore::save(int slot, LayoutPreset preset)
{
    m_presets[static_cast<std::size_t>(slot - 1)] = std::move(preset);
    m_activeSlot = slot;
}

void LayoutPresetStore::markLoaded(int slot)
{
    m_activeSlot = slot;
}

int LayoutPresetStore::activeSlot() const
{
    return m_activeSlot;
}

bool LayoutPresetStore::isDirty(const LayoutPreset& live) const
{
    if (m_activeSlot == 0) {
        return false;
    }
    const LayoutPreset& stored = m_presets[static_cast<std::size_t>(m_activeSlot - 1)];

    return live.stack != stored.stack || live.orientation != stored.orientation || live.weights != stored.weights ||
           live.styles != stored.styles;
}

const std::array<LayoutPreset, LayoutPresetSlots>& LayoutPresetStore::all() const
{
    return m_presets;
}

void LayoutPresetStore::restore(const std::array<LayoutPreset, LayoutPresetSlots>& presets, int activeSlot)
{
    m_presets = presets;
    m_activeSlot = activeSlot;
}

}  // namespace sidescopes
