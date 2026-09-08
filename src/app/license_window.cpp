#include "app/license_window.h"

#include <string>

#include "app/license_notices.h"
#include "imgui.h"

namespace sidescopes {

void LicenseWindow::open()
{
    m_open = true;
}

void LicenseWindow::draw()
{
    if (!m_open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Licenses", &m_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const auto notices = licenseNotices();
    bool changed = false;
    if (ImGui::BeginCombo("Component", notices[m_selected].name)) {
        for (std::size_t i = 0; i < notices.size(); ++i) {
            const bool selected = m_selected == i;
            if (ImGui::Selectable(notices[i].name, selected)) {
                changed = m_selected != i;
                m_selected = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    const auto text = notices[m_selected].text;
    if (ImGui::Button("Copy text")) {
        ImGui::SetClipboardText(std::string(text).c_str());
    }
    ImGui::BeginChild("License text", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (changed) {
        ImGui::SetScrollY(0);
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace sidescopes
