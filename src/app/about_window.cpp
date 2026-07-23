#include "app/about_window.h"

#include "app/imgui_ui.h"
#include "imgui.h"
#include "platform/desktop.h"

namespace sidescopes {

void AboutWindow::open()
{
    m_open = true;
}

void AboutWindow::draw(const VersionInfo& version)
{
    if (!m_open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("About SideScopes", &m_open, ImGuiWindowFlags_NoCollapse);
    // The window title carries the name; the body leads with the version.
    ImGui::Text("Version %s", version.display.c_str());
    if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(version.display.c_str());
    }
    wrappedTooltip("click to copy");
    ImGui::Separator();
    // Clickable link text in the accent color, underlined, opening the
    // destination in the default browser; the tooltip names the URL.
    const auto link = [](const char* text, const char* url) {
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(lo.x, hi.y), ImVec2(hi.x, hi.y), ImGui::GetColorU32(accent));
        if (ImGui::IsItemClicked()) {
            openUrl(url);
        }
        wrappedTooltip(url);
    };
    link("sidescopes.org", "https://sidescopes.org");
    link("github.com/sidescopes/sidescopes", "https://github.com/sidescopes/sidescopes");
    ImGui::Separator();
    ImGui::TextDisabled("GPL-3.0-or-later");
    ImGui::End();
}

}  // namespace sidescopes
