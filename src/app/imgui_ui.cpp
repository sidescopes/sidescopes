#include "app/imgui_ui.h"

#include <algorithm>

#include "app/interface_diagnostics.h"
#include "app/row_layout.h"
#include "imgui.h"
// ErrorCallback is declared in the internal header; ImGui documents it as a
// hook it may promote to the public API rather than as private state.
#include "imgui_internal.h"

namespace sidescopes {
namespace {

// The widest a tooltip may run before wrapping, in 100%-scale points.
constexpr float TooltipWrapWidth = 260.0f;

// What the toolkit calls when it catches a misuse of itself.
void onInterfaceError(ImGuiContext*, void*, const char* message)
{
    reportInterfaceError(message);
}

}  // namespace

void installInterfaceErrorReporting()
{
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
        return;
    }
    context->ErrorCallback = onInterfaceError;
    context->ErrorCallbackUserData = nullptr;
#ifdef NDEBUG
    // ONLY THE WINDOW IS BUILD-GATED, and only here. The callback above is set
    // whatever this is built as, because a recording from a machine nobody
    // here can see is the only way one of these ever reaches us.
    ImGui::GetIO().ConfigErrorRecoveryEnableTooltip = false;
#endif
}

bool interfaceErrorReportingInstalled()
{
    const ImGuiContext* context = ImGui::GetCurrentContext();

    return context != nullptr && context->ErrorCallback == onInterfaceError;
}

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

bool labelledIconButton(const char* id, ImTextureID texture, const char* label, float labelWidth, const char* tooltip)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(labelledIconButtonWidth(labelWidth), iconButtonHeight()));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    // The glyph is seated in the PLAIN button's box at the head of this one,
    // not centred in the widened box, so it lands exactly where the icon of a
    // plain button beside it does.
    const float side = ImGui::GetTextLineHeight();
    const ImVec2 glyph = iconGlyphOrigin(min, ImVec2(min.x + iconButtonWidth(), max.y), side);
    draw->AddImage(texture, glyph, ImVec2(glyph.x + side, glyph.y + side), ImVec2(0, 0), ImVec2(1, 1),
                   ImGui::GetColorU32(ImGuiCol_Text));
    const ImVec2 at(min.x + iconButtonWidth(), min.y + rowTextDrop());
    draw->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), label);
    wrappedTooltip(tooltip);

    return pressed;
}

}  // namespace sidescopes
