#include "app/imgui_ui.h"

#include <algorithm>

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

}  // namespace sidescopes
