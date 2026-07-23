#include "app/color_readout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>

#include "app/color_picker_common.h"
#include "app/color_picker_deck.h"
#include "app/color_picker_difference.h"
#include "app/color_picker_hero.h"
#include "app/hex_font.h"
#include "imgui.h"

namespace sidescopes {
namespace {

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

// The color picker pane: the sampled cursor color as a large
// swatch with its values spelled out three ways at once - 0-255,
// percent, and hex - because matching a reference means never
// converting in your head. Clicking the swatch or the hex line
// copies the hex; the pinned colors (P) ride along as small
// swatches with the same click.
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
