#include "app/color_picker_difference.h"

#include <cmath>
#include <cstdio>

#include "app/hex_font.h"
#include "app/imgui_ui.h"
#include "core/color_lab.h"

namespace sidescopes {
namespace {

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

}  // namespace

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

}  // namespace sidescopes
