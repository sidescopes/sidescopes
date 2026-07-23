#pragma once

#include "app/color_picker_common.h"

namespace sidescopes {

/// The full reference deck when there is room: a scrolling child with a header
/// and one row per pin. A row's remove cross reports through @p removePin, so
/// the pin outlives the loop drawing it.
void drawPickerDeck(const PickerContext& ctx, int& removePin);

/// The chip rail when the pane is too small for the deck: swatches only, click to
/// compare and right-click to manage.
void drawPickerChipRail(const PickerContext& ctx);

}  // namespace sidescopes
