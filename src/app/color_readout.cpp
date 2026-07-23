#include "app/color_readout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "app/imgui_ui.h"
#include "core/color_lab.h"

namespace sidescopes {
namespace {

// The color picker pane: the sampled cursor color as a large
// swatch with its values spelled out three ways at once - 0-255,
// percent, and hex - because matching a reference means never
// converting in your head. Clicking the swatch or the hex line
// copies the hex; the pinned colors (P) ride along as small
// swatches with the same click.
// Managing a pin happens where the pin lives; the app-wide
// native menu yields to this popup.
// Hex codes render in the fixed-width font when one loaded, so
// every code is the same width and columns anchor exactly.
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

// The hex and signed values align in a fixed-width font when the system had
// one; the font is pushed rather than sized by hand so global UI scale applies
// once. A null font falls back to the interface font.
void pushHexFont(ImFont* font)
{
    if (font) {
        ImGui::PushFont(font);
    }
}

void popHexFont(ImFont* font)
{
    if (font) {
        ImGui::PopFont();
    }
}

float hexFontWidth(ImFont* font, const char* text)
{
    pushHexFont(font);
    const float measured = ImGui::CalcTextSize(text).x;
    popHexFont(font);

    return measured;
}

// The monospace face may sit at a size of its own (monospaceFontScale),
// and ImGui top-aligns the items sharing a line, so a value drawn beside
// an interface-font label lands on a different baseline. Sinking the
// value by the ascent difference seats both on the label's baseline; the
// line keeps its own top, so only this item shifts. Rows that position
// their text by hand use pushHexFont directly instead.
float hexFontBaselineDrop(ImFont* font)
{
    const float interfaceAscent = ImGui::GetFontBaked()->Ascent;
    pushHexFont(font);
    const float drop = interfaceAscent - ImGui::GetFontBaked()->Ascent;
    popHexFont(font);

    return drop;
}

void hexFontText(ImFont* font, const char* text)
{
    const float drop = hexFontBaselineDrop(font);
    pushHexFont(font);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + drop);
    ImGui::TextUnformatted(text);
    popHexFont(font);
}

ImVec4 pickerSwatchColor(const FloatColor& source)
{
    return ImVec4(source.r / 255.0f, source.g / 255.0f, source.b / 255.0f, 1.0f);
}

void pinHexOf(const PinBoard& pins, std::size_t index, char* buffer)
{
    std::snprintf(buffer, 8, "#%02X%02X%02X", static_cast<int>(std::lround(pins.color(index).r)),
                  static_cast<int>(std::lround(pins.color(index).g)),
                  static_cast<int>(std::lround(pins.color(index).b)));
}

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

// Tooltip strings shared by the deck header, its rows, and the difference row
// under the hero. One line per column: a paragraph covering all three tells a
// reader about two quantities they did not ask about. The sign gloss is the
// convention every colorimetric tool follows; hue has none, because it runs
// around a circle rather than along an axis.
constexpr const char* PickerDeltaETip = "CIEDE2000 difference from the live color, lower is closer (sRGB assumed)";
constexpr const char* PickerLchTips[3] = {
    "how much lighter (+) or darker (-) the live color is",
    "how much more colorful (+) or duller (-) the live color is",
    "how far the live color's hue has drifted - counts for less when the color is dull",
};
constexpr const char* PickerRgbTips[3] = {
    "how much more (+) or less (-) red the live color is",
    "how much more (+) or less (-) green the live color is",
    "how much more (+) or less (-) blue the live color is",
};

// The live color, its formatted values, and the shared column metrics every
// picker section measures against. pins is borrowed for the length of one draw.
struct PickerContext
{
    FloatColor color;
    PinBoard& pins;
    ImFont* monospaceFont;
    float labelColumn;
    float percentColumn;
    float columnGap;
    float channelStride;
    float hexWidth;
    float lineHeight;
    const char* hex;
};

// The comparator geometry: the size tier, the split against a pinned reference,
// and the on-swatch/solo readout placements the tier admits.
struct PickerHero
{
    bool tiny;
    bool full;
    bool split;
    bool onSwatch;
    bool soloOnSwatch;
    float heroHeight;
    float heroWidth;
    float valuesStart;
    ImVec2 heroOrigin;
    float pad;
    float rowHeight;
    float seamX;
    float blockWidth;
    float blockTop;
};

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

void drawPickerNoColor(const ImVec2& area, float lineHeight)
{
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (area.y - lineHeight) / 2.0f)));
    const char* hint = "no color under the cursor yet";
    const float width = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
    ImGui::TextDisabled("%s", hint);
}

// The split never depends on the tier - only its height does. Reads the cursor,
// so it runs at the hero's start, before anything draws.
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

// The comparator: the live color, split against the selected pin when one is
// loaded. Touching halves make small casts visible where separated swatches
// hide them. Draws into the host window, so it fetches that draw list here.
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

// The CIEDE2000 distance itself, one decimal the way the field quotes it. It
// replaced a friendlier "match percentage": that read as a claim about how
// alike two colors LOOK, which the measure cannot honor at the distances this
// picker works over - it is built and validated for small differences.
void formatDeltaE(float deltaE, char (&value)[8])
{
    std::snprintf(value, sizeof(value), "%.1f", deltaE);
}

// The difference triplet's own metrics. Nothing below the hero lines up with
// it any more - the channel percentages that used to sit above it live in the
// status bar now - so a group is only as wide as it reads: the value sits one
// gap from its label, and the space between groups does the separating.
struct DiffColumns
{
    float label;
    float stride;
    float width;
};

DiffColumns measureDiffColumns(const PickerContext& ctx)
{
    DiffColumns columns{};
    columns.label = ImGui::CalcTextSize("ΔL").x;
    const float value = ImGui::CalcTextSize("-199").x;
    const float group = columns.label + ctx.columnGap + value;
    columns.stride = group + 3.0f * ctx.columnGap;
    columns.width = 2.0f * columns.stride + group;

    return columns;
}

// Three groups - lightness, chroma, hue - each a label and the signed number it
// names, held together by sitting closer to each other than to their neighbors.
void drawPickerDiffTriplet(const PickerContext& ctx, float valuesStart, const float diffValues[3],
                           const DiffColumns& columns)
{
    const char* diffLabels[3] = {"ΔL", "ΔC", "ΔH"};
    for (int component = 0; component < 3; ++component) {
        const float columnStart = valuesStart + static_cast<float>(component) * columns.stride;
        if (component == 0) {
            ImGui::SetCursorPosX(columnStart);
        } else {
            ImGui::SameLine(columnStart);
        }
        ImGui::TextUnformatted(diffLabels[component]);
        wrappedTooltip(PickerLchTips[component]);
        char value[8];
        std::snprintf(value, sizeof(value), "%+d", static_cast<int>(std::lround(diffValues[component])));
        ImGui::SameLine(columnStart + columns.label + ctx.columnGap);
        ImGui::TextUnformatted(value);
        wrappedTooltip(PickerLchTips[component]);
    }
}

// Everything under the hero: where the live color sits relative to the pinned
// one, and how far apart they are overall. The triplet leads and the distance
// closes the line; when the pane cannot seat both, the distance drops to its
// own line rather than pushing the detail off the pane. The live color's hex is not
// here - it changes with every mouse move and cannot be copied, and the deck
// carries the hexes that are worth keeping.
void drawPickerDifferenceRow(const PickerContext& ctx, const ImVec2& area, float valuesStart)
{
    const ColorDifference difference = differenceFrom(labFromSrgb(ctx.pins.comparatorColor()), labFromSrgb(ctx.color));
    char deltaEValue[8];
    formatDeltaE(difference.deltaE, deltaEValue);
    const float deltaEValueX = area.x - hexFontWidth(ctx.monospaceFont, deltaEValue);
    const float deltaELabelX = deltaEValueX - ctx.columnGap - ImGui::CalcTextSize("ΔE").x;
    const float diffValues[3] = {difference.lightness, difference.chroma, difference.hue};
    const DiffColumns columns = measureDiffColumns(ctx);
    const bool tripletShares = valuesStart + columns.width + ctx.columnGap <= deltaELabelX;
    if (tripletShares || valuesStart + columns.width <= area.x) {
        drawPickerDiffTriplet(ctx, valuesStart, diffValues, columns);
    }
    // Right-aligned while it closes the triplet's line; flush left once it has
    // a line of its own, the way the region toolbox wraps.
    float labelX = deltaELabelX;
    float valueX = deltaEValueX;
    if (tripletShares) {
        ImGui::SameLine(labelX);
    } else {
        labelX = valuesStart;
        valueX = labelX + ImGui::CalcTextSize("ΔE").x + ctx.columnGap;
        ImGui::SetCursorPosX(labelX);
    }
    ImGui::TextUnformatted("ΔE");
    wrappedTooltip(PickerDeltaETip);
    ImGui::SameLine(valueX);
    hexFontText(ctx.monospaceFont, deltaEValue);
    wrappedTooltip(PickerDeltaETip);
}

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

// The full reference deck when there is room: a scrolling child with a header
// and one row per pin.
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

// The chip rail when the pane is too small for the deck: swatches only, click to
// compare and right-click to manage.
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

// The color picker pane: the sampled cursor color as a large swatch with its
// values spelled out three ways at once - 0-255, percent, and hex - because
// matching a reference means never converting in your head. Clicking the swatch
// or the hex line copies the hex; the pinned colors (P) ride along.
// Three size tiers, few and spaced so resizing feels like deliberate steps: a
// strip, a compact comparator, the full reference deck. Order never changes -
// comparator, values, pins - and only the comparator absorbs extra height.
// The column metrics of the readout a roomy hero carries on the swatch itself:
// a channel letter, its percentage, and the hex under them.
struct PickerColumns
{
    float label;
    float value;
    float gap;
    float stride;
};

// Every value owns a column sized for its widest form and right-aligns inside
// it - the cure for layouts that twitch as digits come and go. The difference
// row below the hero measures itself (see measureDiffColumns): it carries
// wider labels and signed readings, and nothing lines the two up.
PickerColumns measurePickerColumns()
{
    PickerColumns columns{};
    columns.label = ImGui::CalcTextSize("R").x;
    columns.value = ImGui::CalcTextSize("100%").x;
    columns.gap = ImGui::CalcTextSize(" ").x;
    columns.stride = columns.label + columns.gap + columns.value + 2.0f * columns.gap;

    return columns;
}

}  // namespace

void drawColorPicker(const std::optional<FloatColor>& liveColor, PinBoard& pins, ImFont* monospaceFont)
{
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    if (pins.comparator() >= static_cast<int>(pins.size())) {
        pins.selectComparator(-1);
    }
    if (!liveColor) {
        drawPickerNoColor(area, lineHeight);

        return;
    }
    const FloatColor color = *liveColor;
    const int red = static_cast<int>(std::lround(std::clamp(color.r, 0.0f, 255.0f)));
    const int green = static_cast<int>(std::lround(std::clamp(color.g, 0.0f, 255.0f)));
    const int blue = static_cast<int>(std::lround(std::clamp(color.b, 0.0f, 255.0f)));
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", red, green, blue);
    const PickerColumns columns = measurePickerColumns();
    const float labelColumn = columns.label;
    const float percentColumn = columns.value;
    const float columnGap = columns.gap;
    const float channelStride = columns.stride;
    const float hexWidth = hexFontWidth(monospaceFont, hex);
    const PickerContext ctx{color,     pins,          monospaceFont, labelColumn, percentColumn,
                            columnGap, channelStride, hexWidth,      lineHeight,  hex};

    const PickerHero hero = computePickerHero(ctx, area, style);
    drawPickerHero(ctx, hero);
    if (hero.split && !hero.tiny) {
        drawPickerDifferenceRow(ctx, area, hero.valuesStart);
    }
    if (pins.empty()) {
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.35f));
    int removePin = -1;
    if (hero.full) {
        drawPickerDeck(ctx, removePin);
    } else {
        drawPickerChipRail(ctx);
    }
    if (removePin >= 0) {
        pins.removeAt(static_cast<std::size_t>(removePin));
    }
}

}  // namespace sidescopes
