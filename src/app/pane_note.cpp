#include "app/pane_note.h"

#include <cmath>

#include "app/row_layout.h"

namespace sidescopes {

bool paneNoteFits(float noteWidth, float paneWidth)
{
    return noteWidth + 2.0f * RowSeparation <= paneWidth;
}

void drawPaneNote(const ImVec2& paneMin, const ImVec2& paneSize, const char* note)
{
    const ImVec2 text = ImGui::CalcTextSize(note);
    if (!paneNoteFits(text.x, paneSize.x)) {
        return;
    }
    // Seated on whole pixels, like every other glyph on a row: a half-pixel
    // origin drifts with wherever the window sits.
    const ImVec2 at(std::round(paneMin.x + (paneSize.x - text.x) / 2.0f),
                    std::round(paneMin.y + (paneSize.y - text.y) / 2.0f));
    ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), note);
}

}  // namespace sidescopes
