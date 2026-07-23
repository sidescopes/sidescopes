#pragma once

#include "app/color_picker_common.h"
#include "imgui.h"

namespace sidescopes {

/// Everything under the hero: where the live color sits relative to the pinned
/// one, and how far apart they are overall. The triplet leads and the distance
/// closes the line; when the pane cannot seat both, the distance drops to its
/// own line rather than pushing the detail off the pane. The live color's hex is not
/// here - it changes with every mouse move and cannot be copied, and the deck
/// carries the hexes that are worth keeping.
void drawPickerDifferenceRow(const PickerContext& ctx, const ImVec2& area, float valuesStart);

}  // namespace sidescopes
