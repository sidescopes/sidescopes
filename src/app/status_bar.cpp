#include "app/status_bar.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
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

// Draws one reading in its natural order: the sampled color, then its three
// channel values. The whole group keeps the same trailing inset as the rest of
// the interface rather than using the window edge as the swatch's frame.
void drawCursorReadout(float taken, const std::optional<FloatColor>& color)
{
    // A square as tall as the type's line box looks taller than the text
    // because the button paints its own border, so pull it in one pixel per
    // side. Centre it against the readout glyphs' visible ink rather than the
    // nominal line box: the font reserves uneven ascent/descent space, which
    // otherwise leaves the swatch predictably high beside capitals and digits.
    if (!color) {
        return;
    }
    const FloatColor& live = *color;
    const ReadoutColumns columns = measureReadoutColumns();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float swatch = std::max(1.0f, lineHeight - 2.0f);
    // The clear-region glyph at the far end of the toolbar is inset from its
    // right-aligned button by iconButtonInset(). End the readout on that same
    // vertical line; RowSeparation is a neighbour gap, not an edge margin.
    const float channelsStart = ImGui::GetWindowContentRegionMax().x - iconButtonInset() - columns.width;
    const float swatchStart = channelsStart - RowSeparation - swatch;
    if (swatchStart < taken + RowSeparation) {
        return;
    }
    ImGui::SameLine(swatchStart);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop() + readoutTextInkCenter() - swatch / 2.0f);
    ImGui::ColorButton("##cursor-color", ImVec4(live.r / 255.0f, live.g / 255.0f, live.b / 255.0f, 1.0f),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(swatch, swatch));
    drawReadoutChannels(live, channelsStart, columns);
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

void StatusBar::draw(bool pinsAvailable, const std::optional<FloatColor>& cursorColor)
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
    const float taken = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    drawCursorReadout(taken, cursorColor);
}

// The tool that samples a colour sits beside the colour it samples, not among
// the region tools - those choose what is captured, this one reads it.
void StatusBar::drawPinTool(bool pinsAvailable)
{
    char tooltip[160];
    // The clause left here describes THIS control - why it is greyed - which
    // is what a tooltip on a disabled control is for. The one removed
    // described a gesture on a different target during a mode not yet
    // entered, and read as an instruction to Shift+click the icon itself.
    // The picker says it where it happens, on its own banner.
    std::snprintf(tooltip, sizeof(tooltip), "Pin a color (%s)%s", m_shortcuts.bindings().pinColor.c_str(),
                  pinsAvailable ? "" : " - needs a scope that takes pins");
    const bool pinPressed =
        iconButton("##pin-color", m_icons.textureId(Icon::Pipette, iconPixelSize()), tooltip, !pinsAvailable);
    m_pinToolBounds = ImVec4{ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                             ImGui::GetItemRectMax().y};
    if (pinPressed && pinsAvailable) {
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
