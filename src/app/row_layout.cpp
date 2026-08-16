#include "app/row_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sidescopes {

float iconButtonWidth()
{
    return ImGui::GetTextLineHeight() + 12.0f;
}

float iconButtonHeight()
{
    return ImGui::GetTextLineHeight() + 4.0f;
}

float labelledIconButtonWidth(float labelWidth)
{
    return iconButtonWidth() + labelWidth + iconButtonInset();
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

float readoutTextInkCenter()
{
    const float fontSize = ImGui::GetFontSize();
    ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(fontSize);
    const float scale = baked && baked->Size > 0.0f ? fontSize / baked->Size : 1.0f;
    float top = std::numeric_limits<float>::max();
    float bottom = std::numeric_limits<float>::lowest();
    // Every visible glyph the standing readout can contain. Spaces are not
    // ink and deliberately stay out of the bounds.
    for (const char* character = "RGB0123456789%"; *character != '\0'; ++character) {
        const ImFontGlyph* glyph = baked ? baked->FindGlyph(static_cast<ImWchar>(*character)) : nullptr;
        if (!glyph || !glyph->Visible) {
            continue;
        }
        top = std::min(top, glyph->Y0 * scale);
        bottom = std::max(bottom, glyph->Y1 * scale);
    }
    if (top > bottom) {
        return ImGui::GetTextLineHeight() / 2.0f;
    }

    return (top + bottom) / 2.0f;
}

float statusRowOffset()
{
    const ImGuiStyle& style = ImGui::GetStyle();

    return std::max(0.0f, std::round((style.WindowPadding.y - style.ItemSpacing.y) / 2.0f));
}

bool regionToolboxWraps(float taken, float toolboxWidth, float rowWidth)
{
    return taken + toolboxWidth + RowSeparation > rowWidth;
}

}  // namespace sidescopes
