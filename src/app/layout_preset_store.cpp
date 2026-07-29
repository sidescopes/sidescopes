#include "app/layout_preset_store.h"

#include <cstddef>
#include <utility>

namespace sidescopes {

std::string presetDisplayName(int slot, const LayoutPreset& preset)
{
    return preset.name.empty() ? "Preset " + std::to_string(slot) : preset.name;
}

const LayoutPreset& LayoutPresetStore::at(int slot) const
{
    return m_presets[static_cast<std::size_t>(slot - 1)];
}

LayoutPreset LayoutPresetStore::effective(int slot, const LayoutPreset& defaults) const
{
    const LayoutPreset& stored = at(slot);
    if (!stored.stack.empty()) {
        return stored;
    }
    // A slot holding nothing is still a slot that loads: it restores the
    // arrangement the application opens on. The name is the slot's own
    // whatever it holds, so one named before it was filled keeps its name.
    LayoutPreset preset = defaults;
    preset.name = stored.name;

    return preset;
}

void LayoutPresetStore::save(int slot, LayoutPreset preset)
{
    LayoutPreset& stored = m_presets[static_cast<std::size_t>(slot - 1)];
    // A name outlives the layout it was given to: saving over a slot replaces
    // what it holds, not what it is called. What the live view captures never
    // carries a name, so taking the stored one is also the only way to keep it.
    preset.name = stored.name;
    stored = std::move(preset);
    m_activeSlot = slot;
}

void LayoutPresetStore::markLoaded(int slot)
{
    m_activeSlot = slot;
}

void LayoutPresetStore::rename(int slot, std::string_view name)
{
    m_presets[static_cast<std::size_t>(slot - 1)].name = sanitizedPresetName(name);
}

int LayoutPresetStore::activeSlot() const
{
    return m_activeSlot;
}

bool LayoutPresetStore::isDirty(const LayoutPreset& live, const LayoutPreset& defaults) const
{
    const LayoutPreset stored = effective(m_activeSlot, defaults);

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
    m_activeSlot = activeSlot >= 1 && activeSlot <= LayoutPresetSlots ? activeSlot : 1;
}

}  // namespace sidescopes
