#include "app/imgui_ui.h"

#include <algorithm>
#include <cmath>

#include "app/row_layout.h"
#include "imgui.h"

namespace sidescopes {
namespace {

// The widest a tooltip may run before wrapping, in 100%-scale points.
constexpr float TooltipWrapWidth = 260.0f;

}  // namespace

void wrappedTooltip(const char* text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) || !ImGui::BeginTooltip()) {
        return;
    }
    const float margin = 4.0f * ImGui::GetStyle().WindowPadding.x;
    ImGui::PushTextWrapPos(std::min(ImGui::GetMainViewport()->Size.x - margin, TooltipWrapWidth));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

bool scopeToggleButton(const char* id, const char* letter, bool enabled, const char* tooltip)
{
    const float height = ImGui::GetTextLineHeight() + 4.0f;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(height + 8.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (enabled) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonActive), 3.0f);
    } else if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    const ImU32 color = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 size = ImGui::CalcTextSize(letter);
    const ImVec2 at(std::floor(min.x + (max.x - min.x - size.x) / 2), std::floor(min.y + (max.y - min.y - size.y) / 2));
    draw->AddText(at, color, letter);
    wrappedTooltip(tooltip);
    return pressed;
}

bool iconButton(const char* id, ImTextureID texture, const char* tooltip, bool dimmed)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(iconButtonWidth(), iconButtonHeight()));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    const float side = ImGui::GetTextLineHeight();
    const ImVec2 glyph = iconGlyphOrigin(min, max, side);
    draw->AddImage(texture, glyph, ImVec2(glyph.x + side, glyph.y + side), ImVec2(0, 0), ImVec2(1, 1),
                   ImGui::GetColorU32(ImGuiCol_Text, dimmed ? 0.4f : 1.0f));
    wrappedTooltip(tooltip);

    return pressed;
}

}  // namespace sidescopes
