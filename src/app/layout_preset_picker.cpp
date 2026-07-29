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

// The controls that lead a preset row, and so where its name starts. This list
// leads with buttons where the scope menu leads with a checkbox, so the two
// share the gap that follows them rather than the x it works out to.
float presetLeadingWidth()
{
    return 2.0f * menuRowIconWidth();
}

float presetNameX()
{
    return menuRowNameX(presetLeadingWidth());
}

// The whole width of a row, measured the way the scope menu measures its own:
// the name column, the widest name it will show, room to breathe, the key, and
// the margin the key keeps from the edge.
float presetRowWidth(const std::array<LayoutPreset, LayoutPresetSlots>& presets)
{
    float maxName = 0.0f;
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const std::string name = presetDisplayName(slot, presets[static_cast<std::size_t>(slot - 1)]);
        maxName = std::max(maxName, ImGui::CalcTextSize(name.c_str()).x);
    }
    const float em = ImGui::GetFontSize();
    return presetNameX() + maxName + em * 2.0f + ImGui::CalcTextSize("9").x + menuRowKeyRightPad();
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
    // Read once, before anything on this row can change it: a click lands
    // mid-row and loads the slot, and asking twice would put the band on the
    // row being left and the badge on the row being taken, for one frame.
    const bool chosen = slot == m_presets.activeSlot();
    const float rowTop = ImGui::GetCursorScreenPos().y;
    const bool hovered = menuRowHovered(rowTop);
    drawMenuRowHover(rowTop);
    // The loaded slot is the row that is tinted. Every row's key hint is drawn
    // the same way, so nothing in the row itself marks it.
    if (chosen) {
        drawMenuRowChosen(rowTop);
    }
    ImGui::PushID(slot);
    const float nameWidth = width - presetNameX();
    if (slot == m_renamingSlot) {
        // The pen's column is held open rather than reclaimed, and the field
        // takes exactly the name's own width, so the row keeps its shape and
        // nothing shifts while a name is typed.
        ImGui::Dummy(ImVec2(presetLeadingWidth(), ImGui::GetFrameHeight()));
        ImGui::SameLine(presetNameX());
        drawRenameField(nameWidth, outcome);
        ImGui::PopID();

        return;
    }
    const std::string name = presetDisplayName(slot, m_presets.at(slot));
    // Two controls lead the row - rename it, and save the live layout into it -
    // in the column the scope menu gives its checkbox, so the two lists share a
    // left edge as well as a right one. Both show only on the row under the
    // pointer, since eighteen glyphs standing beside nine names is a wall
    // rather than a list, and both hold their boxes either way, so no name
    // moves as the pointer runs down the list.
    const std::string renameTip = "Rename " + name;
    if (menuRowIconButton("##rename", icons.textureId(Icon::PenLine, iconPixelSize()), renameTip.c_str(), hovered)) {
        beginRename(slot);
    }
    ImGui::SameLine(0.0f, 0.0f);
    const std::string saveTip = "Save the current layout into " + name;
    if (menuRowIconButton("##save", icons.textureId(Icon::Save, iconPixelSize()), saveTip.c_str(), hovered)) {
        outcome = m_presets.save(slot);
    }
    ImGui::SameLine(presetNameX());
    // The row itself only ever loads. Saving was a modifier on this same click
    // and is a button of its own now: a gesture that means two things depending
    // on a held key teaches nobody the second one.
    const ImVec2 rowSize(nameWidth, ImGui::GetFrameHeight());
    if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups, rowSize)) {
        outcome = m_presets.load(slot);
    }
    drawMenuRowAccelerator(std::to_string(slot).c_str(), menuRowKeyRightPad());
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
