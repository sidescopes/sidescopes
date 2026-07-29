#include "app/layout_preset_picker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

#include "app/imgui_ui.h"
#include "app/menu_rows.h"
#include "imgui.h"

namespace sidescopes {
namespace {

// The width every row of the picker shares, from the widest name it will show,
// so the names left-align and the digits right-align to one edge with room to
// breathe between. Measured from the name column rightward, which is where the
// row's Selectable starts.
float presetRowWidth(const std::array<LayoutPreset, LayoutPresetSlots>& presets)
{
    float maxName = 0.0f;
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const std::string name = presetDisplayName(slot, presets[static_cast<std::size_t>(slot - 1)]);
        maxName = std::max(maxName, ImGui::CalcTextSize(name.c_str()).x);
    }
    const float em = ImGui::GetFontSize();

    return maxName + em * 2.0f + ImGui::CalcTextSize("9").x + em * 0.75f;
}

}  // namespace

LayoutPresetPicker::LayoutPresetPicker(LayoutPresetController& presets)
    : m_presets(presets)
{
}

void LayoutPresetPicker::beginRename(int slot)
{
    m_renamingSlot = slot;
    m_renameFocusDue = true;
    const std::string name = presetDisplayName(slot, m_presets.at(slot));
    const std::size_t length = std::min(name.size(), m_renameBuffer.size() - 1);
    std::copy_n(name.begin(), length, m_renameBuffer.begin());
    m_renameBuffer[length] = '\0';
}

LayoutPresetOutcome LayoutPresetPicker::commitRename()
{
    const int slot = m_renamingSlot;
    m_renamingSlot = 0;
    m_renameFocusDue = false;
    if (slot == 0) {
        return LayoutPresetOutcome{};
    }

    return m_presets.rename(slot, std::string{m_renameBuffer.data()});
}

void LayoutPresetPicker::drawRenameField(float width, LayoutPresetOutcome& outcome)
{
    // The field takes the keyboard as it opens and gives it back on Enter or
    // on a click elsewhere; either way what was typed is what lands, so a
    // rename is never lost by clicking away from it. While it holds the
    // keyboard every plain-letter shortcut stands down, so a name can carry
    // any letter or digit.
    ImGui::SetNextItemWidth(width);
    if (m_renameFocusDue) {
        ImGui::SetKeyboardFocusHere();
        m_renameFocusDue = false;
    }
    const bool entered = ImGui::InputText("##rename", m_renameBuffer.data(), m_renameBuffer.size(),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (entered || ImGui::IsItemDeactivated()) {
        outcome = commitRename();
    }
}

void LayoutPresetPicker::drawSlotRow(int slot, float width, IconTextures& icons, LayoutPresetOutcome& outcome)
{
    const LayoutPreset& preset = m_presets.at(slot);
    drawMenuRowHover(ImGui::GetCursorScreenPos().y);
    ImGui::PushID(slot);
    if (slot == m_renamingSlot) {
        // The field spans the name column AND the button's, so the row keeps
        // its width and nothing beside it shifts while a name is typed.
        drawRenameField(width + menuRowIconWidth(), outcome);
        ImGui::PopID();

        return;
    }
    // A radio in the checkbox's column: exactly one slot is loaded, where any
    // number of scopes can be shown, and clicking it loads that slot.
    if (ImGui::RadioButton("##active", slot == m_presets.activeSlot())) {
        outcome = m_presets.load(slot);
    }
    ImGui::SameLine(ImGui::GetFrameHeight() + ImGui::GetFontSize());
    const std::string name = presetDisplayName(slot, preset);
    const ImVec2 rowSize(width, ImGui::GetFrameHeight());
    if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups, rowSize)) {
        outcome = ImGui::GetIO().KeyShift ? m_presets.save(slot) : m_presets.load(slot);
    }
    drawMenuRowAccelerator(std::to_string(slot).c_str(), ImGui::GetFontSize() * 0.75f);
    ImGui::SameLine(0.0f, 0.0f);
    const std::string tooltip = "Rename " + name;
    if (menuRowIconButton("##rename", icons.textureId(Icon::PenLine, iconPixelSize()), tooltip.c_str())) {
        beginRename(slot);
    }
    ImGui::PopID();
}

LayoutPresetOutcome LayoutPresetPicker::draw(IconTextures& icons)
{
    // A chip like the scope letters, leading the row: the label names the
    // active slot (starred once the live layout drifts; "-" when none), and
    // clicking opens the slot list - the mouse mirror of the digit keys. The
    // list is shaped like the scope selector's, because it is the same gesture.
    const int active = m_presets.activeSlot();
    char preview[8] = "-";
    if (active != 0) {
        std::snprintf(preview, sizeof(preview), "%d%s", active, m_presets.activeDirty() ? "*" : "");
    }
    const std::string tooltip =
        active != 0 ? presetDisplayName(active, m_presets.at(active)) + " - digits load, Shift+digits save"
                    : std::string{"Layout presets - digits load, Shift+digits save"};
    if (scopeToggleButton("##preset-picker", preview, false, tooltip.c_str())) {
        ImGui::OpenPopup("##preset-popup");
    }
    const ImVec2 chipMin = ImGui::GetItemRectMin();
    const ImVec2 chipMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(chipMin.x, chipMax.y + 2.0f));
    LayoutPresetOutcome outcome;
    if (ImGui::BeginPopup("##preset-popup")) {
        pushMenuRowStyle();
        const float width = presetRowWidth(m_presets.all());
        for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
            drawSlotRow(slot, width, icons, outcome);
        }
        ImGui::TextDisabled("click loads - Shift+click saves");
        popMenuRowStyle();
        ImGui::EndPopup();
    } else {
        m_renamingSlot = 0;
    }

    return outcome;
}

}  // namespace sidescopes
