#include "app/status_bar.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "app/imgui_ui.h"
#include "app/region_picker.h"
#include "app/row_layout.h"
#include "imgui.h"

namespace sidescopes {
namespace {

void statusRowText(const char* text)
{
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop());
    ImGui::TextUnformatted(text);
}

void drawReadoutChannels(const FloatColor& color, float start, const ReadoutColumns& columns)
{
    const char* labels[3] = {"R", "G", "B"};
    const float channels[3] = {color.r, color.g, color.b};
    for (int channel = 0; channel < 3; ++channel) {
        const float columnStart = start + static_cast<float>(channel) * columns.stride;
        char value[8];
        std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55);
        ImGui::SameLine(columnStart);
        statusRowText(labels[channel]);
        ImGui::SameLine(columnStart + columns.label + columns.gap);
        statusRowText(value);
    }
}

// Draws the swatch inwards from the right corner, preceded by the channel
// readout when the room left by taken - the row's used width so far - allows
// it.
void drawCursorReadout(float taken, const std::optional<FloatColor>& color)
{
    // The colour under the cursor, laid out inwards from its corner: the
    // swatch first, then a named percentage per channel in fixed columns, so
    // no digit coming or going moves anything. The swatch outranks the numbers
    // when the strip runs short, and both give way to whatever already stands
    // on the row.
    if (!color) {
        return;
    }
    const FloatColor& live = *color;
    const float swatch = ImGui::GetTextLineHeight();
    const float swatchStart = ImGui::GetWindowContentRegionMax().x - swatch;
    if (swatchStart < taken + 8.0f) {
        return;
    }
    const ReadoutColumns columns = measureReadoutColumns();
    const float channelsStart = swatchStart - 6.0f - columns.width;
    if (channelsStart >= taken + 8.0f) {
        drawReadoutChannels(live, channelsStart, columns);
    }
    ImGui::SameLine(swatchStart);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop());
    ImGui::ColorButton("##cursor-color", ImVec4(live.r / 255.0f, live.g / 255.0f, live.b / 255.0f, 1.0f),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(swatch, swatch));
}

}  // namespace

float statusBarHeight()
{
    return ImGui::GetStyle().ItemSpacing.y + iconButtonHeight() + statusRowOffset();
}

StatusBar::StatusBar(const ShortcutResolver& shortcuts, RegionPicker& picker, IconTextures& icons)
    : m_shortcuts(shortcuts),
      m_picker(picker),
      m_icons(icons)
{
}

void StatusBar::draw(bool regionSelected, bool pinsAvailable, const std::optional<FloatColor>& cursorColor)
{
    // The reserved strip under the panes. Output owns its own row - it never
    // paints over the scopes' pixels. Idle, the row spans corner to corner:
    // the pin tool holds the left, the live swatch the right, and the channel
    // readout gathers against the swatch. A message clears the row and takes
    // it whole, so a line that only shows for a moment is not something to be
    // picked out from among the standing furniture.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + statusRowOffset());
    // A full-height anchor opens the row before anything stands on it, so the
    // line's origin never depends on which of them is showing. Without it the
    // first element to be placed sets the origin, and a message - shorter than
    // the tool - dragged everything after it down.
    ImGui::Dummy(ImVec2(0.0f, iconButtonHeight()));
    if (!m_message.empty() && glfwGetTime() <= m_until) {
        // Indented to the tool's glyph rather than to the content edge: the
        // row keeps one left edge whichever of the two is standing on it.
        ImGui::SameLine(0.0f, iconButtonInset());
        statusRowText(m_message.c_str());

        return;
    }
    ImGui::SameLine(0.0f, 0.0f);
    drawPinTool(pinsAvailable);
    float taken = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    if (!regionSelected) {
        taken = drawNoRegionNote(taken);
    }
    drawCursorReadout(taken, cursorColor);
}

// What the row says while the scopes are reading nothing: the one gesture that
// fills them, in the register of standing furniture rather than of a warning.
// The scopes themselves stay legible - graticule, markers and readout all keep
// working - so this teaches rather than explains a fault.
float StatusBar::drawNoRegionNote(float taken)
{
    char note[80];
    std::snprintf(note, sizeof(note), "Draw a region (%s) to fill the scopes",
                  shortcutLabel(m_shortcuts.bindings().drawRegion).c_str());
    if (!statusNoteFits(taken, ImGui::CalcTextSize(note).x, ImGui::GetTextLineHeight(),
                        ImGui::GetWindowContentRegionMax().x)) {
        return taken;
    }
    ImGui::SameLine(taken + RowSeparation);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop());
    ImGui::TextDisabled("%s", note);

    return ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
}

// The tool that samples a colour sits beside the colour it samples, not among
// the region tools - those choose what is captured, this one reads it.
void StatusBar::drawPinTool(bool pinsAvailable)
{
    char tooltip[160];
    std::snprintf(tooltip, sizeof(tooltip), "Pin a color (%s)%s", m_shortcuts.bindings().pinColor.c_str(),
                  pinsAvailable ? " - Shift+click a color to pin several" : " - needs a scope that takes pins");
    if (iconButton("##pin-color", m_icons.textureId(Icon::Pipette, iconPixelSize()), tooltip, !pinsAvailable) &&
        pinsAvailable) {
        m_picker.request(RegionPickerMode::PinColor);
    }
}

/// A status message covers the live readout, so it clears itself. Long enough to
/// read a sentence without looking away from the photo.
constexpr double StatusDwellSeconds = 5.0;

void StatusBar::setStatus(std::string message)
{
    m_message = std::move(message);
    m_until = glfwGetTime() + StatusDwellSeconds;
}

double StatusBar::redrawDueSeconds() const
{
    return m_until;
}

}  // namespace sidescopes
