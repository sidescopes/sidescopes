#include "app/menu_rows.h"

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
