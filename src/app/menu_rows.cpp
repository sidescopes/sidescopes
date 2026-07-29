#include "app/menu_rows.h"

#include <cmath>

#include "app/imgui_ui.h"
#include "imgui.h"

namespace sidescopes {
namespace {

// How many of each the style push leaves behind, so the pop cannot disagree.
constexpr int PushedVars = 3;
constexpr int PushedColors = 4;

// How much of the accent the chosen row's band carries. Enough that the row
// reads as chosen at a glance, and see-through enough that a chosen row under
// the pointer is brighter again - so the two states are told apart rather than
// one hiding the other.
constexpr float ChosenBandOpacity = 0.55f;

// How strongly the key hint is drawn, on top of the theme's already-dim
// disabled text. It states which key reaches a row; it is not a control, and
// it shares the row's right edge with one that is.
constexpr float AcceleratorOpacity = 0.75f;

// A row's highlight band.
struct RowBand
{
    ImVec2 min;
    ImVec2 max;
};

// The band a whole row highlights across: a little above and below the row,
// and nearly the popup's full width - just shy of the border, past the content
// into the window padding - for a native-menu-style highlight edge to edge.
RowBand rowBand(float rowTopY)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = style.ItemSpacing.y * 0.3f;
    const float inset = style.WindowPadding.x * 0.4f;
    const ImVec2 windowPos = ImGui::GetWindowPos();

    return RowBand{ImVec2(windowPos.x + inset, rowTopY - pad),
                   ImVec2(windowPos.x + ImGui::GetWindowSize().x - inset, rowTopY + ImGui::GetFrameHeight() + pad)};
}

// Where a key of @p keySize sits over the row just laid down: right-aligned
// @p rightPad in from that row's edge, on the row's own centre line.
ImVec2 acceleratorOrigin(const ImVec2& keySize, float rightPad)
{
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();

    return ImVec2(rowMax.x - rightPad - keySize.x, rowMin.y + (rowMax.y - rowMin.y - keySize.y) * 0.5f);
}

}  // namespace

float menuRowNameX()
{
    return ImGui::GetFrameHeight() + ImGui::GetFontSize();
}

float menuRowKeyRightPad()
{
    return ImGui::GetFontSize() * 0.75f;
}

void pushMenuRowStyle()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 frameBg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    // The frame padding is pulled right down so a checkbox sits near the text
    // height rather than dwarfing its label.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, ImGui::GetFontSize() * 0.05f));
    // Row-tall Selectables carry their label centred, so the whole row is one
    // target and one drag handle, not just the text line.
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    // Roomier rows than the theme default, so short rows are not condensed and
    // the hover band has space to breathe.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, ImGui::GetFontSize() * 0.7f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frameBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frameBg);
}

void popMenuRowStyle()
{
    ImGui::PopStyleColor(PushedColors);
    ImGui::PopStyleVar(PushedVars);
}

void drawMenuRowHover(float rowTopY)
{
    if (ImGui::GetDragDropPayload() != nullptr) {
        return;
    }
    const RowBand band = rowBand(rowTopY);
    if (!ImGui::IsMouseHoveringRect(band.min, band.max)) {
        return;
    }
    ImGui::GetWindowDrawList()->AddRectFilled(band.min, band.max, ImGui::GetColorU32(ImGuiCol_ButtonHovered),
                                              ImGui::GetStyle().FrameRounding);
}

void drawMenuRowChosen(float rowTopY)
{
    const RowBand band = rowBand(rowTopY);
    ImGui::GetWindowDrawList()->AddRectFilled(band.min, band.max,
                                              ImGui::GetColorU32(ImGuiCol_ButtonActive, ChosenBandOpacity),
                                              ImGui::GetStyle().FrameRounding);
}

float menuRowIconWidth()
{
    return ImGui::GetFrameHeight() + ImGui::GetFontSize() * 0.5f;
}

bool menuRowIconButton(const char* id, ImTextureID texture, const char* tooltip)
{
    // Sized to the row rather than to the toolbar: iconButton's box is a fixed
    // amount taller than a line of text, which in a popup whose frame padding
    // is pulled right down would make the rows carrying one visibly taller
    // than the rest. The GLYPH keeps the toolbar's size, so it reads as the
    // same icon at the same weight.
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(menuRowIconWidth(), ImGui::GetFrameHeight()));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), ImGui::GetStyle().FrameRounding);
    }
    const float side = ImGui::GetTextLineHeight();
    const ImVec2 glyph(std::round(min.x + (max.x - min.x - side) / 2.0f),
                       std::round(min.y + (max.y - min.y - side) / 2.0f));
    draw->AddImage(texture, glyph, ImVec2(glyph.x + side, glyph.y + side), ImVec2(0, 0), ImVec2(1, 1),
                   ImGui::GetColorU32(ImGuiCol_Text));
    wrappedTooltip(tooltip);

    return pressed;
}

void drawMenuRowAccelerator(const char* key, float rightPad)
{
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    const ImVec2 at = acceleratorOrigin(ImGui::CalcTextSize(key), rightPad);
    ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled, AcceleratorOpacity), key);
}

void drawMenuRowChosenKey(const char* key, float rightPad)
{
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    // The chosen row is marked in the cell its key already occupies rather
    // than in a column of its own, which is what buys the space between the
    // key and the button after it. A filled badge also differs from the plain
    // hint in SHAPE and in contrast, not only in colour: a coloured marker
    // alone says nothing to a reader who cannot tell it from the unmarked one.
    const ImVec2 keySize = ImGui::CalcTextSize(key);
    const ImVec2 at = acceleratorOrigin(keySize, rightPad);
    const float padX = ImGui::GetFontSize() * 0.35f;
    const float padY = ImGui::GetFontSize() * 0.15f;
    const ImVec2 min(at.x - padX, at.y - padY);
    const ImVec2 max(at.x + keySize.x + padX, at.y + keySize.y + padY);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonActive), (max.y - min.y) * 0.5f);
    draw->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), key);
}

}  // namespace sidescopes
