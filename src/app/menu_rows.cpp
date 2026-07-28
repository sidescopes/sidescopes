#include "app/menu_rows.h"

#include <cmath>

#include "app/imgui_ui.h"
#include "imgui.h"

namespace sidescopes {
namespace {

// How many of each the style push leaves behind, so the pop cannot disagree.
constexpr int PushedVars = 3;
constexpr int PushedColors = 4;

}  // namespace

void pushMenuRowStyle()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 frameBg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    // The frame padding is pulled right down so a checkbox sits near the text
    // height rather than dwarfing its label.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, ImGui::GetFontSize() * 0.05f));
    // Row-tall Selectables carry their label centred, so the whole row is one
    // target and one drag handle, not just the text line.
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    // Roomier rows than the theme default, so short rows are not condensed and
    // the hover band has space to breathe.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, ImGui::GetFontSize() * 0.7f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frameBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frameBg);
}

void popMenuRowStyle()
{
    ImGui::PopStyleColor(PushedColors);
    ImGui::PopStyleVar(PushedVars);
}

void drawMenuRowHover(float rowTopY)
{
    if (ImGui::GetDragDropPayload() != nullptr) {
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = style.ItemSpacing.y * 0.3f;
    const float inset = style.WindowPadding.x * 0.4f;
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 barMin(windowPos.x + inset, rowTopY - pad);
    const ImVec2 barMax(windowPos.x + ImGui::GetWindowSize().x - inset, rowTopY + ImGui::GetFrameHeight() + pad);
    if (!ImGui::IsMouseHoveringRect(barMin, barMax)) {
        return;
    }
    ImGui::GetWindowDrawList()->AddRectFilled(barMin, barMax, ImGui::GetColorU32(ImGuiCol_ButtonHovered),
                                              style.FrameRounding);
}

float menuRowIconWidth()
{
    return ImGui::GetFrameHeight() + ImGui::GetFontSize() * 0.5f;
}

bool menuRowIconButton(const char* id, ImTextureID texture, const char* tooltip)
{
    // Sized to the row rather than to the toolbar: iconButton's box is a fixed
    // amount taller than a line of text, which in a popup whose frame padding
    // is pulled right down would make the rows carrying one visibly taller
    // than the rest. The GLYPH keeps the toolbar's size, so it reads as the
    // same icon at the same weight.
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(menuRowIconWidth(), ImGui::GetFrameHeight()));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), ImGui::GetStyle().FrameRounding);
    }
    const float side = ImGui::GetTextLineHeight();
    const ImVec2 glyph(std::round(min.x + (max.x - min.x - side) / 2.0f),
                       std::round(min.y + (max.y - min.y - side) / 2.0f));
    draw->AddImage(texture, glyph, ImVec2(glyph.x + side, glyph.y + side), ImVec2(0, 0), ImVec2(1, 1),
                   ImGui::GetColorU32(ImGuiCol_Text));
    wrappedTooltip(tooltip);

    return pressed;
}

void drawMenuRowAccelerator(const char* key, float rightPad)
{
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    const ImVec2 keySize = ImGui::CalcTextSize(key);
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const ImVec2 at(rowMax.x - rightPad - keySize.x, rowMin.y + (rowMax.y - rowMin.y - keySize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), key);
}

}  // namespace sidescopes
