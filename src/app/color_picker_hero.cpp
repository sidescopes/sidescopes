#include "app/color_picker_hero.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>

#include "app/hex_font.h"
#include "app/imgui_ui.h"

namespace sidescopes {
namespace {

// Ink and its legibility shadow both follow the color beneath: dark ink over a
// light shadow on light colors, light ink over a dark shadow on dark ones.
ImU32 pickerLabelInk(const FloatColor& under)
{
    const float luma = (54.0f * under.r + 183.0f * under.g + 19.0f * under.b) / 256.0f;

    return luma > 140.0f ? IM_COL32(0, 0, 0, 170) : IM_COL32(255, 255, 255, 180);
}

ImU32 pickerLabelShadow(const FloatColor& under)
{
    const float luma = (54.0f * under.r + 183.0f * under.g + 19.0f * under.b) / 256.0f;

    return luma > 140.0f ? IM_COL32(255, 255, 255, 64) : IM_COL32(0, 0, 0, 115);
}

// One string over its shadow, in an optional font: the hex rows pass the
// fixed-width font, everything else draws in the interface font. AddText
// is top-aligned, so a passed font sinks by the ascent difference and
// shares the interface font's baseline.
void swatchText(ImDrawList* draw, const ImVec2& pos, ImU32 ink, ImU32 shadow, const char* text, ImFont* font = nullptr)
{
    const ImVec2 seated(pos.x, pos.y + (font ? hexFontBaselineDrop(font) : 0.0f));
    if (font) {
        ImGui::PushFont(font, font->LegacySize);
    }
    draw->AddText(ImVec2(seated.x, seated.y + 1.0f), shadow, text);
    draw->AddText(seated, ink, text);
    if (font) {
        ImGui::PopFont();
    }
}

// One hero half's readout: labels in a fixed column, percentages right-aligned
// to the seam, hex on the fourth row. The left half mirrors the right.
void drawPickerSwatchBlock(ImDrawList* draw, const PickerContext& ctx, const PickerHero& hero, const FloatColor& swatch,
                           bool leftHalf)
{
    const ImU32 ink = pickerLabelInk(swatch);
    const ImU32 shadow = pickerLabelShadow(swatch);
    const float channels[3] = {swatch.r, swatch.g, swatch.b};
    const char* labels[3] = {"R", "G", "B"};
    for (int channel = 0; channel < 3; ++channel) {
        const float y = hero.blockTop + static_cast<float>(channel) * hero.rowHeight;
        char value[8];
        std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55f);
        const float valueWidth = ImGui::CalcTextSize(value).x;
        const float labelX = leftHalf ? hero.seamX - hero.pad - hero.blockWidth : hero.seamX + hero.pad;
        const float valueX =
            leftHalf ? hero.seamX - hero.pad - valueWidth : hero.seamX + hero.pad + hero.blockWidth - valueWidth;
        swatchText(draw, ImVec2(labelX, y), ink, shadow, labels[channel]);
        swatchText(draw, ImVec2(valueX, y), ink, shadow, value);
    }
    char blockHex[8];
    std::snprintf(blockHex, sizeof(blockHex), "#%02X%02X%02X",
                  static_cast<int>(std::lround(std::clamp(swatch.r, 0.0f, 255.0f))),
                  static_cast<int>(std::lround(std::clamp(swatch.g, 0.0f, 255.0f))),
                  static_cast<int>(std::lround(std::clamp(swatch.b, 0.0f, 255.0f))));
    const float hexY = hero.blockTop + 3.0f * hero.rowHeight;
    const float hexX = leftHalf ? hero.seamX - hero.pad - ctx.hexWidth : hero.seamX + hero.pad;
    swatchText(draw, ImVec2(hexX, hexY), ink, shadow, blockHex, ctx.monospaceFont);
}

// Solo, one line along the swatch foot carries the same reading when the hero is
// tall enough and the whole line fits across it.
void drawPickerSoloReadout(ImDrawList* draw, const PickerContext& ctx, const PickerHero& hero)
{
    const ImU32 ink = pickerLabelInk(ctx.color);
    const ImU32 shadow = pickerLabelShadow(ctx.color);
    draw->AddText(ImVec2(hero.heroOrigin.x + 5, hero.heroOrigin.y + 3), ink, "LIVE");
    const float baselineY = hero.heroOrigin.y + hero.heroHeight - hero.pad - hero.rowHeight;
    const float startX = hero.heroOrigin.x + hero.pad;
    const float channels[3] = {ctx.color.r, ctx.color.g, ctx.color.b};
    const char* labels[3] = {"R", "G", "B"};
    for (int channel = 0; channel < 3; ++channel) {
        const float columnStart = startX + static_cast<float>(channel) * ctx.channelStride;
        char value[8];
        std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55f);
        swatchText(draw, ImVec2(columnStart, baselineY), ink, shadow, labels[channel]);
        const float valueX =
            columnStart + ctx.labelColumn + ctx.columnGap + ctx.percentColumn - ImGui::CalcTextSize(value).x;
        swatchText(draw, ImVec2(valueX, baselineY), ink, shadow, value);
    }
    swatchText(draw, ImVec2(startX + 3.0f * ctx.channelStride, baselineY), ink, shadow, ctx.hex, ctx.monospaceFont);
}

}  // namespace

void drawPickerNoColor(const ImVec2& area, float lineHeight)
{
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (area.y - lineHeight) / 2.0f)));
    const char* hint = "no color under the cursor yet";
    const float width = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
    ImGui::TextDisabled("%s", hint);
}

PickerHero computePickerHero(const PickerContext& ctx, const ImVec2& area, const ImGuiStyle& style)
{
    PickerHero hero{};
    hero.tiny = area.y < 120.0f;
    hero.full = area.y >= 240.0f;
    // With nothing pinned there is no deck to reserve for, and an uncapped
    // comparator absorbs the pane instead of leaving a dead black field.
    const float deckReserve = ctx.pins.empty() ? 0.0f : (hero.full ? 4.0f * ctx.lineHeight : ctx.lineHeight);
    const float reserved = 2.0f * ctx.lineHeight + deckReserve + style.ItemSpacing.y;
    const float heroCap = ctx.pins.empty() ? area.y : (hero.full ? 220.0f : 64.0f);
    hero.heroHeight = hero.tiny ? ctx.lineHeight * 1.5f : std::clamp(area.y - reserved, 48.0f, heroCap);
    hero.split = ctx.pins.hasComparator();
    hero.heroWidth = area.x;
    hero.heroOrigin = ImGui::GetCursorScreenPos();
    hero.valuesStart = ImGui::GetCursorPosX();
    hero.pad = 8.0f;
    hero.rowHeight = ctx.lineHeight;
    hero.seamX = hero.heroOrigin.x + hero.heroWidth / 2.0f;
    hero.blockWidth = ctx.labelColumn + ctx.columnGap + ctx.percentColumn;
    const float blockBottom = hero.heroOrigin.y + hero.heroHeight - hero.pad;
    hero.blockTop = blockBottom - 4.0f * hero.rowHeight;
    // A split half carries the readout only when it is tall enough for four rows
    // and wide enough to hold the block clear of its corner label; hex can be
    // the widest row, so it drives the width test.
    const float blockExtent = std::max(hero.blockWidth, ctx.hexWidth);
    hero.onSwatch = hero.split && hero.heroHeight >= 6.0f * ctx.lineHeight &&
                    hero.heroWidth / 2.0f >= blockExtent + 2.0f * hero.pad + ImGui::CalcTextSize("LIVE").x;
    hero.soloOnSwatch = !hero.split && hero.heroHeight >= 3.5f * ctx.lineHeight &&
                        3.0f * ctx.channelStride + ctx.hexWidth + 2.0f * hero.pad <= hero.heroWidth;

    return hero;
}

void drawPickerHero(const PickerContext& ctx, const PickerHero& hero)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // The live swatch renders but never copies: the sample follows the cursor,
    // so a click would destroy the very color it shows.
    ImGui::ColorButton("##picker-live", pickerSwatchColor(ctx.color),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(hero.split ? hero.heroWidth / 2.0f : hero.heroWidth, hero.heroHeight));
    if (hero.split) {
        char pinHex[8];
        pinHexOf(ctx.pins, static_cast<std::size_t>(ctx.pins.comparator()), pinHex);
        ImGui::SameLine(0.0f, 0.0f);
        if (ImGui::ColorButton("##picker-reference", pickerSwatchColor(ctx.pins.comparatorColor()),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(hero.heroWidth / 2.0f, hero.heroHeight))) {
            ImGui::SetClipboardText(pinHex);
        }
        char pinnedTip[48];
        std::snprintf(pinnedTip, sizeof(pinnedTip), "pinned  %s - click to copy", pinHex);
        wrappedTooltip(pinnedTip);
        if (!hero.tiny) {
            draw->AddText(ImVec2(hero.heroOrigin.x + 5, hero.heroOrigin.y + 3), pickerLabelInk(ctx.color), "LIVE");
            const float pinLabel = ImGui::CalcTextSize("PIN").x;
            draw->AddText(ImVec2(hero.heroOrigin.x + hero.heroWidth - pinLabel - 5, hero.heroOrigin.y + 3),
                          pickerLabelInk(ctx.pins.comparatorColor()), "PIN");
            if (hero.onSwatch) {
                drawPickerSwatchBlock(draw, ctx, hero, ctx.color, true);
                drawPickerSwatchBlock(draw, ctx, hero, ctx.pins.comparatorColor(), false);
            }
        }
    }
    if (hero.soloOnSwatch) {
        drawPickerSoloReadout(draw, ctx, hero);
    }
}

}  // namespace sidescopes
