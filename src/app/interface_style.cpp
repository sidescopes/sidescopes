#include "app/interface_style.h"

#include "imgui.h"

namespace sidescopes {

void applyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.WindowPadding = ImVec2(10, 8);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    ImVec4* colors = style.Colors;
    const ImVec4 background(0.0f, 0.0f, 0.0f, 1.0f);
    const ImVec4 panel(0.13f, 0.13f, 0.14f, 1.0f);
    const ImVec4 hovered(0.20f, 0.20f, 0.22f, 1.0f);
    const ImVec4 active(0.26f, 0.27f, 0.30f, 1.0f);
    const ImVec4 accent(0.28f, 0.42f, 0.65f, 1.0f);
    colors[ImGuiCol_WindowBg] = background;
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.11f, 0.98f);
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgActive] = background;
    colors[ImGuiCol_FrameBg] = panel;
    colors[ImGuiCol_FrameBgHovered] = hovered;
    colors[ImGuiCol_FrameBgActive] = active;
    colors[ImGuiCol_Button] = panel;
    colors[ImGuiCol_ButtonHovered] = hovered;
    colors[ImGuiCol_ButtonActive] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.36f, 0.52f, 0.78f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.70f, 0.95f, 1.0f);
    colors[ImGuiCol_Text] = ImVec4(0.86f, 0.86f, 0.88f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.53f, 1.0f);
}

void applyInterfaceScale(float scale)
{
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    applyTheme();
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
}

}  // namespace sidescopes
