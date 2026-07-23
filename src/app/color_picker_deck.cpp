#include "app/color_picker_deck.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "app/hex_font.h"
#include "app/imgui_ui.h"
#include "core/color_lab.h"
#include "imgui.h"

namespace sidescopes {
namespace {

// Managing a pin happens where the pin lives; the app-wide
// native menu yields to this popup.
void drawPinnedMenu(PinBoard& pins)
{
    if (!ImGui::BeginPopup("##pinned-menu")) {
        return;
    }
    if (pins.managed() >= 0 && pins.managed() < static_cast<int>(pins.size())) {
        const std::size_t chosen = static_cast<std::size_t>(pins.managed());
        char chosenHex[8];
        std::snprintf(
            chosenHex, sizeof(chosenHex), "#%02X%02X%02X", static_cast<int>(std::lround(pins.color(chosen).r)),
            static_cast<int>(std::lround(pins.color(chosen).g)), static_cast<int>(std::lround(pins.color(chosen).b)));
        if (ImGui::MenuItem(chosenHex)) {
            ImGui::SetClipboardText(chosenHex);
        }
        if (ImGui::MenuItem("Remove")) {
            pins.removeAt(chosen);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear All")) {
            pins.clear();
        }
    }
    ImGui::EndPopup();
}

// The deck's admitted numeric groups and the right edge of every visible
// column, walked once so the header and every row share them.
struct DeckLayout
{
    float swatchX;
    float hexX;
    bool showDeltaE;
    bool showLch;
    bool showRgb;
    float deltaERight;
    float lchRight[3];
    float rgbRight[3];
    float deltaETypical;
    float lchTypical;
    float rgbTypical;
};

// Progressive disclosure: the distance first, then L/C/H, then R/G/B, each group
// admitted only if the whole block still clears the hex column.
void admitDeckGroups(float leftPartEnd, float deckWidth, float blockGap, float columnGap, float deltaECol,
                     float lchGroupWidth, float rgbGroupWidth, DeckLayout& layout)
{
    float numericBlockWidth = 0.0f;
    const auto admitGroup = [&](float groupWidth) {
        const float tentative = numericBlockWidth + (numericBlockWidth > 0.0f ? 3.0f * columnGap : 0.0f) + groupWidth;
        if (leftPartEnd + blockGap + tentative <= deckWidth) {
            numericBlockWidth = tentative;

            return true;
        }

        return false;
    };
    layout.showDeltaE = admitGroup(deltaECol);
    layout.showLch = layout.showDeltaE && admitGroup(lchGroupWidth);
    layout.showRgb = layout.showLch && admitGroup(rgbGroupWidth);
}

// Right edges of every visible column, walked left to right from the block's
// left edge. The block anchors just past the hex column, not the far edge.
void computeDeckRights(float leftPartEnd, float blockGap, float columnGap, float deltaECol, const float lchCol[3],
                       const float rgbCol[3], DeckLayout& layout)
{
    float walk = leftPartEnd + blockGap;
    if (layout.showDeltaE) {
        layout.deltaERight = walk + deltaECol;
        walk = layout.deltaERight;
    }
    if (layout.showLch) {
        walk += 3.0f * columnGap;
        for (int column = 0; column < 3; ++column) {
            if (column > 0) {
                walk += 2.0f * columnGap;
            }
            layout.lchRight[column] = walk + lchCol[column];
            walk = layout.lchRight[column];
        }
    }
    if (layout.showRgb) {
        walk += 3.0f * columnGap;
        for (int column = 0; column < 3; ++column) {
            if (column > 0) {
                walk += 2.0f * columnGap;
            }
            layout.rgbRight[column] = walk + rgbCol[column];
            walk = layout.rgbRight[column];
        }
    }
}

// Fixed left part: cross, swatch, hex; then the numeric columns, each sized for
// its widest value or its header, whichever is wider. Width decisions use the
// child's own content region, so the last column is not clipped under a bar.
DeckLayout computeDeckLayout(const PickerContext& ctx, float deckWidth)
{
    DeckLayout layout{};
    const float hexColumn = hexFontWidth(ctx.monospaceFont, "#DDDDDD");
    layout.swatchX = ctx.lineHeight + 3.0f * ctx.columnGap;
    layout.hexX = layout.swatchX + ctx.lineHeight + ctx.columnGap;
    const float leftPartEnd = layout.hexX + hexColumn;
    const float deltaECol = std::max(hexFontWidth(ctx.monospaceFont, "199.9"), ImGui::CalcTextSize("ΔE").x);
    // Every numeric column in the deck is a difference against the live color,
    // so each carries the delta the hero row's absolute channels do without.
    const char* lchLabels[3] = {"ΔL", "ΔC", "ΔH"};
    const char* rgbLabels[3] = {"ΔR", "ΔG", "ΔB"};
    float lchCol[3];
    float rgbCol[3];
    for (int column = 0; column < 3; ++column) {
        lchCol[column] = std::max(hexFontWidth(ctx.monospaceFont, "+199"), ImGui::CalcTextSize(lchLabels[column]).x);
        rgbCol[column] = std::max(hexFontWidth(ctx.monospaceFont, "+100%"), ImGui::CalcTextSize(rgbLabels[column]).x);
    }
    const float lchGroupWidth = lchCol[0] + lchCol[1] + lchCol[2] + 2.0f * (2.0f * ctx.columnGap);
    const float rgbGroupWidth = rgbCol[0] + rgbCol[1] + rgbCol[2] + 2.0f * (2.0f * ctx.columnGap);
    const float blockGap = 3.0f * ctx.columnGap;
    admitDeckGroups(leftPartEnd, deckWidth, blockGap, ctx.columnGap, deltaECol, lchGroupWidth, rgbGroupWidth, layout);
    computeDeckRights(leftPartEnd, blockGap, ctx.columnGap, deltaECol, lchCol, rgbCol, layout);
    // Each header label centers over the ink of a typical value - a sign and two
    // digits, right-aligned - rather than over the column box.
    layout.lchTypical = hexFontWidth(ctx.monospaceFont, "+34");
    layout.rgbTypical = hexFontWidth(ctx.monospaceFont, "+34%");
    layout.deltaETypical = hexFontWidth(ctx.monospaceFont, "77.7");

    return layout;
}

void drawPickerDeckHeader(const DeckLayout& layout)
{
    // Nothing sits above the cross, swatch, or hex; the first admitted column
    // sets the cursor and the rest ride SameLine.
    bool firstHeader = true;
    const auto headerCell = [&](float colRight, float typicalWidth, const char* label, const char* tip) {
        const float headerX = colRight - (typicalWidth + ImGui::CalcTextSize(label).x) / 2.0f;
        if (firstHeader) {
            ImGui::SetCursorPosX(headerX);
            firstHeader = false;
        } else {
            ImGui::SameLine(headerX);
        }
        ImGui::TextUnformatted(label);
        wrappedTooltip(tip);
    };
    if (layout.showDeltaE) {
        headerCell(layout.deltaERight, layout.deltaETypical, "ΔE", PickerDeltaETip);
    }
    if (layout.showLch) {
        headerCell(layout.lchRight[0], layout.lchTypical, "ΔL", PickerLchTips[0]);
        headerCell(layout.lchRight[1], layout.lchTypical, "ΔC", PickerLchTips[1]);
        headerCell(layout.lchRight[2], layout.lchTypical, "ΔH", PickerLchTips[2]);
    }
    if (layout.showRgb) {
        headerCell(layout.rgbRight[0], layout.rgbTypical, "ΔR", PickerRgbTips[0]);
        headerCell(layout.rgbRight[1], layout.rgbTypical, "ΔG", PickerRgbTips[1]);
        headerCell(layout.rgbRight[2], layout.rgbTypical, "ΔB", PickerRgbTips[2]);
    }
}

// The remove cross leads the row: a frameless glyph, quiet gray until hovered,
// red at the moment of intent. Draws inside the deck child's own draw list.
void drawDeckRowCross(std::size_t index, float lineHeight, int& removePin)
{
    char closeId[24];
    std::snprintf(closeId, sizeof(closeId), "##unpin-%d", static_cast<int>(index));
    if (ImGui::InvisibleButton(closeId, ImVec2(lineHeight, lineHeight))) {
        removePin = static_cast<int>(index);
    }
    wrappedTooltip("remove this pin");
    const ImVec2 crossLo = ImGui::GetItemRectMin();
    const ImVec2 crossHi = ImGui::GetItemRectMax();
    const ImVec2 crossCenter = ImVec2((crossLo.x + crossHi.x) / 2.0f, (crossLo.y + crossHi.y) / 2.0f);
    const float arm = lineHeight * 0.17f;
    const ImU32 crossInk = ImGui::IsItemHovered() ? IM_COL32(235, 90, 90, 255) : IM_COL32(150, 150, 150, 180);
    ImDrawList* cross = ImGui::GetWindowDrawList();
    cross->AddLine(ImVec2(crossCenter.x - arm, crossCenter.y - arm), ImVec2(crossCenter.x + arm, crossCenter.y + arm),
                   crossInk, 1.4f);
    cross->AddLine(ImVec2(crossCenter.x - arm, crossCenter.y + arm), ImVec2(crossCenter.x + arm, crossCenter.y - arm),
                   crossInk, 1.4f);
}

// The swatch: click loads it into the comparator, right-click manages. Its
// selection ring fetches the deck child's draw list.
void drawDeckRowSwatch(const PickerContext& ctx, std::size_t index, const DeckLayout& layout)
{
    char pinId[24];
    std::snprintf(pinId, sizeof(pinId), "##pin-%d", static_cast<int>(index));
    const bool selected = static_cast<int>(index) == ctx.pins.comparator();
    ImGui::SameLine(layout.swatchX);
    if (ImGui::ColorButton(pinId, pickerSwatchColor(ctx.pins.color(index)),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(ctx.lineHeight, ctx.lineHeight))) {
        ctx.pins.selectComparator(selected ? -1 : static_cast<int>(index));
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ctx.pins.manage(static_cast<int>(index));
        ImGui::OpenPopup("##pinned-menu");
    }
    wrappedTooltip(selected ? "click to unload from the comparator" : "click to compare against the live color");
    if (selected) {
        // A white ring inside a dark one reads on any pin color; the gold rim
        // vanished on skin tones.
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        ImDrawList* ringDraw = ImGui::GetWindowDrawList();
        ringDraw->AddRect(ImVec2(lo.x - 1, lo.y - 1), ImVec2(hi.x + 1, hi.y + 1), IM_COL32(0, 0, 0, 220), 0.0f, 0,
                          2.0f);
        ringDraw->AddRect(lo, hi, IM_COL32(235, 235, 235, 235), 0.0f, 0, 1.5f);
    }
}

// Hex then the numeric columns, each value right-aligned in its column and
// vertically centered like the hex.
void drawDeckRowValues(const PickerContext& ctx, std::size_t index, const DeckLayout& layout, float rowPosY,
                       float textDrop)
{
    // textDrop tops the text where the row's framed neighbours top their
    // labels; the ascent difference then seats the fixed-width text on
    // the same baseline as the cross button beside it.
    const float seat = textDrop + hexFontBaselineDrop(ctx.monospaceFont);
    char pinHex[8];
    pinHexOf(ctx.pins, index, pinHex);
    ImGui::SameLine(layout.hexX);
    ImGui::SetCursorPosY(rowPosY + seat);
    pushHexFont(ctx.monospaceFont);
    ImGui::TextUnformatted(pinHex);
    popHexFont(ctx.monospaceFont);
    if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(pinHex);
    }
    wrappedTooltip("click to copy");
    const ColorDifference pinDiff = differenceFrom(labFromSrgb(ctx.pins.color(index)), labFromSrgb(ctx.color));
    const auto numericCell = [&](float colRight, const char* value, const char* tip) {
        ImGui::SameLine(colRight - hexFontWidth(ctx.monospaceFont, value));
        ImGui::SetCursorPosY(rowPosY + seat);
        pushHexFont(ctx.monospaceFont);
        ImGui::TextUnformatted(value);
        popHexFont(ctx.monospaceFont);
        wrappedTooltip(tip);
    };
    if (layout.showDeltaE) {
        char deltaEValue[8];
        formatDeltaE(pinDiff.deltaE, deltaEValue);
        numericCell(layout.deltaERight, deltaEValue, PickerDeltaETip);
    }
    if (layout.showLch) {
        const float lchValues[3] = {pinDiff.lightness, pinDiff.chroma, pinDiff.hue};
        for (int column = 0; column < 3; ++column) {
            char value[8];
            std::snprintf(value, sizeof(value), "%+d", static_cast<int>(std::lround(lchValues[column])));
            numericCell(layout.lchRight[column], value, PickerLchTips[column]);
        }
    }
    if (layout.showRgb) {
        const float pinChannels[3] = {ctx.pins.color(index).r, ctx.pins.color(index).g, ctx.pins.color(index).b};
        const float liveChannels[3] = {ctx.color.r, ctx.color.g, ctx.color.b};
        for (int column = 0; column < 3; ++column) {
            char value[8];
            std::snprintf(value, sizeof(value), "%+.0f%%", (liveChannels[column] - pinChannels[column]) / 2.55f);
            numericCell(layout.rgbRight[column], value, PickerRgbTips[column]);
        }
    }
}

void drawPickerDeckRow(const PickerContext& ctx, std::size_t index, const DeckLayout& layout, int& removePin)
{
    const float rowPosY = ImGui::GetCursorPosY();
    const float textDrop = (ctx.lineHeight - ImGui::GetFontSize()) / 2.0f;
    drawDeckRowCross(index, ctx.lineHeight, removePin);
    drawDeckRowSwatch(ctx, index, layout);
    drawDeckRowValues(ctx, index, layout, rowPosY, textDrop);
}

}  // namespace

void drawPickerDeck(const PickerContext& ctx, int& removePin)
{
    ImGui::BeginChild("##pin-deck", ImVec2(0, 0));
    const float deckWidth = ImGui::GetContentRegionAvail().x;
    const DeckLayout layout = computeDeckLayout(ctx, deckWidth);
    drawPickerDeckHeader(layout);
    for (std::size_t index = 0; index < ctx.pins.size(); ++index) {
        drawPickerDeckRow(ctx, index, layout, removePin);
    }
    drawPinnedMenu(ctx.pins);
    ImGui::EndChild();
}

void drawPickerChipRail(const PickerContext& ctx)
{
    for (std::size_t index = 0; index < ctx.pins.size(); ++index) {
        char pinId[24];
        std::snprintf(pinId, sizeof(pinId), "##pin-%d", static_cast<int>(index));
        char pinHex[8];
        pinHexOf(ctx.pins, index, pinHex);
        const bool selected = static_cast<int>(index) == ctx.pins.comparator();
        if (index > 0) {
            ImGui::SameLine(0.0f, 4.0f);
        }
        if (ImGui::ColorButton(pinId, pickerSwatchColor(ctx.pins.color(index)),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(ctx.lineHeight, ctx.lineHeight))) {
            ctx.pins.selectComparator(selected ? -1 : static_cast<int>(index));
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ctx.pins.manage(static_cast<int>(index));
            ImGui::OpenPopup("##pinned-menu");
        }
        char deckTip[64];
        std::snprintf(deckTip, sizeof(deckTip), "%s - click to compare, right-click to manage", pinHex);
        wrappedTooltip(deckTip);
        if (selected) {
            const ImVec2 lo = ImGui::GetItemRectMin();
            const ImVec2 hi = ImGui::GetItemRectMax();
            ImDrawList* ringDraw = ImGui::GetWindowDrawList();
            ringDraw->AddRect(ImVec2(lo.x - 1, lo.y - 1), ImVec2(hi.x + 1, hi.y + 1), IM_COL32(0, 0, 0, 220), 0.0f, 0,
                              2.0f);
            ringDraw->AddRect(lo, hi, IM_COL32(235, 235, 235, 235), 0.0f, 0, 1.5f);
        }
    }
    drawPinnedMenu(ctx.pins);
}

}  // namespace sidescopes
