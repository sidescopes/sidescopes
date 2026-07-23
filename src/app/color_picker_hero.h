#pragma once

#include "app/color_picker_common.h"
#include "imgui.h"

namespace sidescopes {

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

/// The hero's standing-by state, centred in @p area: nothing has been sampled
/// yet, so there is no color to compare against.
void drawPickerNoColor(const ImVec2& area, float lineHeight);

/// The split never depends on the tier - only its height does. Reads the cursor,
/// so it runs at the hero's start, before anything draws.
[[nodiscard]] PickerHero computePickerHero(const PickerContext& ctx, const ImVec2& area, const ImGuiStyle& style);

/// The comparator: the live color, split against the selected pin when one is
/// loaded. Touching halves make small casts visible where separated swatches
/// hide them. Draws into the host window, so it fetches that draw list here.
void drawPickerHero(const PickerContext& ctx, const PickerHero& hero);

}  // namespace sidescopes
