#include "app/toolbar.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "app/imgui_ui.h"
#include "app/region_picker.h"
#include "imgui.h"
#include "platform/face_detection.h"

namespace sidescopes {
namespace {

// Per-scope toolbar chrome, keyed by id: the button id, display name, and
// tooltip suffix. The shortcut is resolved by id through the resolver.
struct ScopeChrome
{
    const char* buttonId;
    const char* name;
    const char* extra;
};

ScopeChrome scopeChromeFor(std::string_view id)
{
    if (id == VectorscopeScopeId) {
        return {"##toggle-vectorscope", "Vectorscope", ""};
    }
    if (id == WaveformScopeId) {
        return {"##toggle-waveform", "Waveform", "; styles in the right-click menu"};
    }
    if (id == ParadeScopeId) {
        return {"##toggle-waveform-parade", "RGB parade", ""};
    }
    if (id == HistogramScopeId) {
        return {"##toggle-histogram", "Histogram", ""};
    }

    return {"##toggle-color-picker", "Color picker", ""};
}

}  // namespace

Toolbar::Toolbar(const ScopeRegistry& registry, ScopeView& view, const ShortcutResolver& shortcuts,
                 RegionPicker& picker, IconTextures& icons)
    : m_registry(registry),
      m_view(view),
      m_shortcuts(shortcuts),
      m_picker(picker),
      m_icons(icons)
{
}

PaneRenderOutcome Toolbar::drawScopeToggles(bool stackModifier)
{
    // Scope toggles are letter chips; switching is the common case, so a plain
    // click shows one scope alone and Shift stacks.
    PaneRenderOutcome outcome;
    char tooltip[96];
    const auto scopeTooltip = [&](const char* name, const std::string& binding, const char* extra) {
        std::snprintf(tooltip, sizeof(tooltip), "%s - %s to switch, Shift+%s to stack%s", name, binding.c_str(),
                      binding.c_str(), extra);
        return tooltip;
    };
    for (const HostScope& scope : m_registry.scopes()) {
        if (scope.letter == 0) {
            continue;
        }
        const ScopeChrome chrome = scopeChromeFor(scope.id);
        const char letter[2] = {scope.letter, '\0'};
        if (scopeToggleButton(chrome.buttonId, letter, m_view.stack().shows(scope.id),
                              scopeTooltip(chrome.name, m_shortcuts.bindingFor(scope.id), chrome.extra))) {
            outcome.chosenScope = ScopeChoice{scope.id, stackModifier};
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::SameLine(0.0f, 8.0f);

    return outcome;
}

void Toolbar::placeRegionToolbox() const
{
    // The brief note after an attached window closed out from under its region
    // stays on the left, by the scopes cluster, clear of the toolbox.
    if (glfwGetTime() < m_attachNoticeUntil) {
        ImGui::TextDisabled("%s", m_attachNotice.c_str());
        ImGui::SameLine(0.0f, 8.0f);
    }
    // The region toolbox is a constant-width cluster: state dims a tool, it
    // never removes one, so the row reflows only when the WINDOW changes -
    // not when the scope stack does. Right-aligned while it shares the row
    // with the scopes; flush left when it wraps to a row of its own. Narrow
    // windows are the tall beside-the-editor shape, which has the height
    // for a second row; wide strips keep one row.
    const int iconCount = 3 + (supportsFaceDetection() ? 1 : 0);
    const float chip = ImGui::GetTextLineHeight() + 12.0f;
    const float width = static_cast<float>(iconCount) * chip + static_cast<float>(iconCount - 1) * 2.0f;
    const float right = ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetCursorPosX() + width + 8.0f > right) {
        ImGui::NewLine();
    } else {
        ImGui::SetCursorPosX(right - width);
    }
}

PaneRenderOutcome Toolbar::drawRegionToolIcons(bool regionIsFullScreen)
{
    PaneRenderOutcome outcome;
    char tooltip[96];
    std::snprintf(tooltip, sizeof(tooltip), "Draw a region (%s)", m_shortcuts.bindings().drawRegion.c_str());
    const int iconPx = iconPixelSize();
    placeRegionToolbox();
    if (iconButton("##draw-region", m_icons.textureId(Icon::Pencil, iconPx), tooltip)) {
        m_picker.request(RegionPickerMode::DrawGlobal);
    }
    ImGui::SameLine(0.0f, 2.0f);
    std::snprintf(tooltip, sizeof(tooltip), "Attach to a window (%s) - click the window or draw inside it",
                  m_shortcuts.bindings().attachWindow.c_str());
    if (iconButton("##attach-window", m_icons.textureId(Icon::SquarePen, iconPx), tooltip)) {
        m_picker.request(RegionPickerMode::AttachWindow);
    }
    ImGui::SameLine(0.0f, 2.0f);
    // The face tool sits last among the region tools, before the reset. It
    // is always available where the platform detects faces: whether any
    // face is on screen is the picker overlay's answer to give, not the
    // toolbar's.
    if (supportsFaceDetection()) {
        std::snprintf(tooltip, sizeof(tooltip), "Attach to a face (%s)", m_shortcuts.bindings().attachFace.c_str());
        if (iconButton("##attach-face", m_icons.textureId(Icon::User, iconPx), tooltip)) {
            m_picker.request(RegionPickerMode::AttachFace);
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    const bool fullAlready = regionIsFullScreen;
    if (iconButton("##full-screen", m_icons.textureId(Icon::Expand, iconPx),
                   fullAlready ? "Reset to full screen (Esc) - already full" : "Reset to full screen (Esc)",
                   fullAlready) &&
        !fullAlready) {
        outcome.resetToFullScreen = true;
    }
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::NewLine();

    return outcome;
}

void Toolbar::showAttachNotice(std::string message)
{
    m_attachNotice = std::move(message);
    m_attachNoticeUntil = glfwGetTime() + 5.0;
}

}  // namespace sidescopes
