#include "app/layout_preset_picker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

#include "app/imgui_ui.h"
#include "app/menu_rows.h"
#include "app/row_layout.h"
#include "imgui.h"

namespace sidescopes {
namespace {

// The widest thing the toolbar button's label can ever say: the last slot,
// drifted. The button reserves this width whatever it is showing, so the star
// arriving and leaving never moves the row.
constexpr const char* WidestPresetLabel = "9*";

// The line under the list, which teaches the two gestures a row answers to.
constexpr const char* PresetFooter = "click loads - Shift+click saves";

// What parts the shortcut digit from the rename button after it. The button's
// box begins exactly where the row's own width ends, so this gap is the whole
// distance between a hint and a control - and it was nothing to speak of when
// the digit sat a fraction of a line in from that edge.
float presetKeyRightPad()
{
    return ImGui::GetFontSize() * 1.75f;
}

// The width every row of the picker shares, from the widest name it will show,
// so the names left-align and the digits right-align to one edge with room to
// breathe between.
float presetRowWidth(const std::array<LayoutPreset, LayoutPresetSlots>& presets)
{
    float maxName = 0.0f;
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const std::string name = presetDisplayName(slot, presets[static_cast<std::size_t>(slot - 1)]);
        maxName = std::max(maxName, ImGui::CalcTextSize(name.c_str()).x);
    }
    const float em = ImGui::GetFontSize();
    const float named = maxName + em * 2.0f + ImGui::CalcTextSize("9").x + presetKeyRightPad();

    // Never narrower than the footer beneath it: the rows then reach the
    // popup's own edge, and each rename button lands flush right rather than
    // partway across with an empty strip beside it.
    return std::max(named, ImGui::CalcTextSize(PresetFooter).x - menuRowIconWidth());
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
    const float rowTop = ImGui::GetCursorScreenPos().y;
    drawMenuRowHover(rowTop);
    // The loaded slot is the row that is tinted, not a marker in a column of
    // its own: it says which one at a glance, needs nothing explaining it, and
    // leaves the row's whole width to the name.
    if (slot == m_presets.activeSlot()) {
        drawMenuRowChosen(rowTop);
    }
    ImGui::PushID(slot);
    if (slot == m_renamingSlot) {
        // The field spans the name column AND the button's, so the row keeps
        // its width and nothing beside it shifts while a name is typed.
        drawRenameField(width + menuRowIconWidth(), outcome);
        ImGui::PopID();

        return;
    }
    const std::string name = presetDisplayName(slot, m_presets.at(slot));
    const ImVec2 rowSize(width, ImGui::GetFrameHeight());
    if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups, rowSize)) {
        outcome = ImGui::GetIO().KeyShift ? m_presets.save(slot) : m_presets.load(slot);
    }
    drawMenuRowAccelerator(std::to_string(slot).c_str(), presetKeyRightPad());
    ImGui::SameLine(0.0f, 0.0f);
    const std::string tooltip = "Rename " + name;
    if (menuRowIconButton("##rename", icons.textureId(Icon::PenLine, iconPixelSize()), tooltip.c_str())) {
        beginRename(slot);
    }
    ImGui::PopID();
}

LayoutPresetOutcome LayoutPresetPicker::draw(IconTextures& icons)
{
    // A sibling of the scope selector, standing after it: a preset IS a set of
    // scopes, so the compound control follows the thing it is composed of, and
    // scopes are switched constantly where a preset is loaded occasionally.
    // The same button, carrying the slot it is on rather than a bare digit,
    // starred once the live layout drifts from that slot. Clicking opens the
    // list - the mouse mirror of the digit keys - which is shaped like the
    // scope selector's, because it is the same gesture.
    const int active = m_presets.activeSlot();
    char label[8] = "";
    std::snprintf(label, sizeof(label), "%d%s", active, m_presets.activeDirty() ? "*" : "");
    const std::string tooltip = presetDisplayName(active, m_presets.at(active)) + " - digits load, Shift+digits save";
    const float labelWidth = ImGui::CalcTextSize(WidestPresetLabel).x;
    if (labelledIconButton("##preset-picker", icons.textureId(Icon::PanelsTopLeft, iconPixelSize()), label, labelWidth,
                           tooltip.c_str())) {
        ImGui::OpenPopup("##preset-popup");
    }
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(buttonMin.x, buttonMax.y + 2.0f));
    LayoutPresetOutcome outcome;
    if (ImGui::BeginPopup("##preset-popup")) {
        pushMenuRowStyle();
        const float width = presetRowWidth(m_presets.all());
        for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
            drawSlotRow(slot, width, icons, outcome);
        }
        ImGui::TextDisabled("%s", PresetFooter);
        popMenuRowStyle();
        ImGui::EndPopup();
    } else {
        m_renamingSlot = 0;
    }
    // Each control on this row keeps the gap after itself, so the order they
    // are drawn in is the only thing that decides the order they appear in.
    ImGui::SameLine(0.0f, RowSeparation);

    return outcome;
}

}  // namespace sidescopes
