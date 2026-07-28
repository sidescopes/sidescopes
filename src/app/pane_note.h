#pragma once

#include "imgui.h"

/// A line of standing text laid over an empty scope, where its trace would be.
///
/// A scope reading nothing is still an instrument - graticule, markers and
/// readout all keep working - so what it says belongs on the instrument itself
/// rather than beside a control that is unaffected.
namespace sidescopes {

/// Whether a note @p noteWidth wide can stand centred in a pane @p paneWidth
/// wide, clear of the pane's edges.
///
/// The note gives way rather than being cut: a pane dragged narrow keeps the
/// instrument whole and drops the words, which can be read from the toolbox
/// above instead.
[[nodiscard]] bool paneNoteFits(float noteWidth, float paneWidth);

/// Draws @p note centred on the pane at @p paneMin spanning @p paneSize, in the
/// disabled text colour - or nothing where the pane is too narrow to hold it.
///
/// It goes straight to the window's draw list, so it lays over the graticule
/// already drawn there without moving the layout cursor.
void drawPaneNote(const ImVec2& paneMin, const ImVec2& paneSize, const char* note);

}  // namespace sidescopes
