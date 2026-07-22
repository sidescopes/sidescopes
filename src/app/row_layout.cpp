#include "app/row_layout.h"

#include <algorithm>
#include <cmath>

namespace sidescopes {

float iconButtonWidth()
{
    return ImGui::GetTextLineHeight() + 12.0f;
}

float iconButtonHeight()
{
    return ImGui::GetTextLineHeight() + 4.0f;
}

float iconButtonInset()
{
    return std::round((iconButtonWidth() - ImGui::GetTextLineHeight()) / 2.0f);
}

ImVec2 iconGlyphOrigin(const ImVec2& min, const ImVec2& max, float side)
{
    return ImVec2(std::round(min.x + (max.x - min.x - side) / 2.0f), std::round(min.y + (max.y - min.y - side) / 2.0f));
}

float rowTextDrop()
{
    return std::round((iconButtonHeight() - ImGui::GetTextLineHeight()) / 2.0f);
}

ReadoutColumns measureReadoutColumns()
{
    ReadoutColumns columns{};
    columns.label = ImGui::CalcTextSize("R").x;
    columns.gap = ImGui::CalcTextSize(" ").x;
    const float group = columns.label + columns.gap + ImGui::CalcTextSize("100%").x;
    columns.stride = group + 2.0f * columns.gap;
    columns.width = 2.0f * columns.stride + group;

    return columns;
}

float statusRowOffset()
{
    const ImGuiStyle& style = ImGui::GetStyle();

    return std::max(0.0f, std::round((style.WindowPadding.y - style.ItemSpacing.y) / 2.0f));
}

}  // namespace sidescopes
