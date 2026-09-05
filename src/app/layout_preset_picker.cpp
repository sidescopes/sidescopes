#include "app/layout_preset_picker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

#include "app/imgui_ui.h"
#include "app/menu_rows.h"
#include "app/row_layout.h"
#include "app/shortcut_resolver.h"
#include "imgui.h"

namespace sidescopes {
namespace {

// The widest thing the toolbar button's label can ever say. The button reserves
// this width whatever it is showing, so no slot number moves the row.
constexpr const char* WidestPresetLabel = "9";

// The controls that lead a preset row, and so where its name starts. This list
// leads with buttons where the scope menu leads with a checkbox, so the two
// share the gap that follows them rather than the x it works out to.
float presetLeadingWidth()
{
    return menuRowIconWidth();
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
    // The loaded slot is the row that is tinted, and the row whose own controls
    // stand at full strength while every other row's recede - so which slot is
    // loaded survives being unable to tell the tint from the background.
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
        // Placed by the text inside it rather than by its box, so the name
        // does not step sideways as the rename opens on it.
        ImGui::SameLine(renameFieldX(presetNameX()));
        drawRenameField(renameFieldWidth(nameWidth), outcome);
        ImGui::PopID();

        return;
    }
    const std::string name = presetDisplayName(slot, m_presets.at(slot));
    // The pen leads the row, in the column the scope menu gives its checkbox,
    // so the two lists share a left edge as well as a right one. It is always
    // there and it recedes: nine pens at full strength beside nine names is a
    // wall rather than a list, and nine HIDDEN ones leave the column they
    // still have to reserve empty down the whole list.
    //
    // It comes up on the row under the pointer, and on the loaded row, which
    // is what tells that row apart from the rest by something other than the
    // hue of the band behind it.
    const std::string renameTip = "Rename " + quotedPresetName(slot, m_presets.at(slot));
    if (menuRowIconButton("##rename", icons.textureId(Icon::PenLine, iconPixelSize()), renameTip.c_str(),
                          hovered || chosen)) {
        beginRename(slot);
    }
    ImGui::SameLine(presetNameX());
    // Clicking a row loads it; saving is the toolbar button's action.
    // And loading ends the errand, so the default close-on-select stands. The
    // keep-open flag this row used to carry belongs to the scope selector,
    // whose rows are checkboxes a user toggles several of in one visit; here
    // it left the list standing over the layout it had just changed.
    const ImVec2 rowSize(nameWidth, ImGui::GetFrameHeight());
    if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_None, rowSize)) {
        outcome = m_presets.load(slot);
    }
    drawMenuRowAccelerator(std::to_string(slot).c_str(), menuRowKeyRightPad());
    ImGui::PopID();
}

void LayoutPresetPicker::drawSaveButton(IconTextures& icons, LayoutPresetOutcome& outcome)
{
    // Dark and inert while the slot already holds what is on screen, the way
    // the region toolbox stands its clear tool down with nothing to clear.
    // This is also the ONLY drift indicator: the preset button carried a star
    // for the same fact, and two marks for one thing is the clutter the list
    // was cleared of.
    const int active = m_presets.activeSlot();
    const bool dirty = m_presets.activeDirty();
    const std::string tooltip = "Save the layout (" + saveChordLabel() + ")" + (dirty ? "" : " - nothing to save");
    if (iconButton("##save-preset", icons.textureId(Icon::Save, iconPixelSize()), tooltip.c_str(), !dirty) && dirty) {
        outcome = m_presets.saveInto(active);
    }
    ImGui::SameLine(0.0f, RowSeparation);
}

LayoutPresetOutcome LayoutPresetPicker::draw(IconTextures& icons)
{
    // A sibling of the scope selector, standing after it: a preset IS a set of
    // scopes, so the compound control follows the thing it is composed of, and
    // scopes are switched constantly where a preset is loaded occasionally.
    // The same button, carrying the slot it is on rather than a bare digit.
    // Clicking opens the list - the mouse mirror of the digit keys - which is
    // shaped like the scope selector's, because it is the same gesture.
    const int active = m_presets.activeSlot();
    char label[8] = "";
    std::snprintf(label, sizeof(label), "%d", active);
    // No preset name in it. The button already shows the slot, the list shows
    // the name against its row, and the status line names it on load and on
    // save; a fourth telling was redundant, and it made a constant string
    // depend on what somebody typed.
    const char* tooltip = "Load a layout preset (1-9)";
    const float labelWidth = ImGui::CalcTextSize(WidestPresetLabel).x;
    if (labelledIconButton("##preset-picker", icons.textureId(Icon::PanelsTopLeft, iconPixelSize()), label, labelWidth,
                           tooltip)) {
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
    drawSaveButton(icons, outcome);

    return outcome;
}

}  // namespace sidescopes
