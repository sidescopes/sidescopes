#include "app/toolbar.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/imgui_ui.h"
#include "app/menu_rows.h"
#include "app/region_picker.h"
#include "app/row_layout.h"
#include "imgui.h"
#include "platform/face_detection.h"

namespace sidescopes {
namespace {

// The scope's display name for the menu: from its descriptor, with the one
// host scope (the colour picker, which has none) named explicitly.
const char* scopeDisplayName(const HostScope& scope)
{
    return scope.descriptor != nullptr ? scope.descriptor->name : "Color picker";
}

// The drag-and-drop payload tag for a dragged scope row: its index in the menu
// order travels as the payload.
constexpr const char* ScopeRowPayload = "ss_scope_row";

// Draws the drop insertion bar across the open popup at height @p y: a thin rule
// in the gap the dragged scope would land in, in a quiet near-white rather than
// the loud drag-drop accent.
void drawInsertionBar(float y)
{
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const float pad = ImGui::GetStyle().WindowPadding.x;
    const ImVec2 left(windowPos.x + pad, y);
    const ImVec2 right(windowPos.x + ImGui::GetWindowSize().x - pad, y);
    ImGui::GetWindowDrawList()->AddLine(left, right, ImGui::GetColorU32(ImGuiCol_Text, 0.85f), 1.0f);
}

// The insertion slot (0..count) the cursor is over, in a list of @p count rows
// pitched @p advance apart from @p listTopY with @p spacing between them: the
// count of rows whose centre the cursor has passed. One computation for the
// whole list, so exactly one line is drawn - never two rows claiming a gap.
int insertionGap(float listTopY, int count, float advance, float spacing)
{
    const float mouseY = ImGui::GetMousePos().y;
    int gap = 0;
    for (int n = 0; n < count; ++n) {
        if (mouseY > listTopY + static_cast<float>(n) * advance + (advance - spacing) * 0.5f) {
            gap = n + 1;
        }
    }

    return gap;
}

// The y of the insertion line for slot @p gap: the centre of the @p spacing
// strip between the rows either side, from their measured @p advance - so the
// line sits midway between two rows whatever their true height.
float insertionBarY(float listTopY, int gap, float advance, float spacing)
{
    return listTopY + static_cast<float>(gap) * advance - spacing * 0.5f;
}

// While a scope is being dragged, draws the single insertion line and, over the
// @p count rows from @p listTop spanning @p width, a full-list drop catch that
// lands the reorder wherever the release falls: live only during the drag, it
// overlays the rows without disturbing their clicks, which the drag suspends.
// @return The row lifted and the gap it was dropped in, or nothing while no
// drop landed.
std::optional<std::pair<int, int>> scopeDrop(ImVec2 listTop, float width, int count)
{
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    if (drag == nullptr || !drag->IsDataType(ScopeRowPayload)) {
        return std::nullopt;
    }
    const ImVec2 resume = ImGui::GetCursorScreenPos();
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    // The real per-row pitch from the laid-out list, so the line tracks the rows
    // whatever their height rather than a computed guess.
    const float advance = (resume.y - listTop.y) / static_cast<float>(count);
    const int gap = insertionGap(listTop.y, count, advance, spacing);
    drawInsertionBar(insertionBarY(listTop.y, gap, advance, spacing));
    ImGui::SetCursorScreenPos(listTop);
    ImGui::InvisibleButton("##scope-drop", ImVec2(width, resume.y - listTop.y));
    int from = -1;
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(ScopeRowPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (payload != nullptr) {
            from = *static_cast<const int*>(payload->Data);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetCursorScreenPos(resume);
    if (from < 0) {
        return std::nullopt;
    }

    return std::make_pair(from, gap);
}

// Seats the constant-width region toolbox on the row the scope selector opened.
//
// The toolbox is a constant-width cluster: state dims a tool, it never removes
// one, so the row reflows only when the WINDOW changes - not when the scope
// stack does, and not when something transient stands beside it. Right-aligned
// while it shares the row with the scopes; flush left when it wraps to a row of
// its own. Narrow windows are the tall beside-the-editor shape, which has the
// height for a second row; wide strips keep one row.
void placeRegionToolbox()
{
    const int iconCount = 3 + (supportsFaceDetection() ? 1 : 0);
    const float chip = ImGui::GetTextLineHeight() + 12.0f;
    const float width = static_cast<float>(iconCount) * chip + static_cast<float>(iconCount - 1) * 2.0f;
    const float right = ImGui::GetWindowContentRegionMax().x;
    if (regionToolboxWraps(ImGui::GetCursorPosX(), width, right)) {
        ImGui::NewLine();
    } else {
        ImGui::SetCursorPosX(right - width);
    }
}

}  // namespace

Toolbar::Toolbar(const ScopeRegistry& registry, ScopeView& view, const ShortcutResolver& shortcuts,
                 RegionPicker& picker, IconTextures& icons)
    : m_registry(registry),
      m_view(view),
      m_shortcuts(shortcuts),
      m_picker(picker),
      m_icons(icons)
{
}

PaneRenderOutcome Toolbar::drawScopeToggles(bool)
{
    // The scope selector, in place of the letter chips: an icon button whose
    // popup checklists every scope. Same shape as the preset picker - the popup
    // opens just below the button.
    PaneRenderOutcome outcome;
    const int iconPx = iconPixelSize();
    if (iconButton("##scopes", m_icons.textureId(Icon::ChartColumn, iconPx), "Scopes - choose which to show")) {
        ImGui::OpenPopup("##scopes-popup");
    }
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(buttonMin.x, buttonMax.y + 2.0f));
    if (ImGui::BeginPopup("##scopes-popup")) {
        appendScopeMenu(outcome);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 8.0f);

    return outcome;
}

Toolbar::ScopeMenuColumns Toolbar::scopeColumns() const
{
    // One width for every row, from the widest name and key, so names left-align
    // and keys right-align to a common edge with room to breathe between.
    float maxName = 0.0f;
    float maxKey = 0.0f;
    for (const HostScope& scope : m_registry.scopes()) {
        maxName = std::max(maxName, ImGui::CalcTextSize(scopeDisplayName(scope)).x);
        maxKey = std::max(maxKey, ImGui::CalcTextSize(m_shortcuts.bindingFor(scope.id).c_str()).x);
    }
    const float em = ImGui::GetFontSize();
    ScopeMenuColumns cols;
    cols.nameX = ImGui::GetFrameHeight() + em;  // checkbox, then a full space to the name
    cols.rightPad = em * 0.75f;                 // the keys keep clear of the border
    cols.width = cols.nameX + maxName + em * 2.0f + maxKey + cols.rightPad;

    return cols;
}

void Toolbar::appendScopeMenu(PaneRenderOutcome& outcome)
{
    // Every registered scope, once, in the order the user keeps them in -
    // which is what a checkbox is for: the list holds still while several are
    // checked and unchecked, and a row moves only when it is dragged. Every
    // row toggles, never solos, so the menu stays open across several clicks.
    pushMenuRowStyle();
    const ScopeMenuColumns cols = scopeColumns();
    const std::vector<std::string>& order = m_view.order().ids();
    if (order.size() > 1) {
        ImGui::TextDisabled("drag to reorder");
    }
    const ImVec2 listTop = ImGui::GetCursorScreenPos();
    for (int n = 0; n < static_cast<int>(order.size()); ++n) {
        drawScopeRow(order[n], n, cols, outcome);
    }
    if (const auto drop = scopeDrop(listTop, cols.width, static_cast<int>(order.size()))) {
        outcome.preferencesSaveDue = m_view.reorderScopes(drop->first, drop->second);
    }
    popMenuRowStyle();
}

void Toolbar::drawScopeRow(const std::string& id, int index, const ScopeMenuColumns& cols, PaneRenderOutcome& outcome)
{
    // A checkbox that shows or hides the scope, then its row-tall name as the
    // drag handle - so a reorder is never read as a toggle. The insertion line
    // and the drop are handled once, past the loop, from the whole list's
    // geometry.
    drawMenuRowHover(ImGui::GetCursorScreenPos().y);
    ImGui::PushID(id.c_str());
    bool on = m_view.stack().shows(id);
    if (ImGui::Checkbox("##shown", &on)) {
        outcome.chosenScope = ScopeChoice{id, true};
    }
    ImGui::SameLine(cols.nameX);
    const char* name = scopeName(id);
    // A click on the name toggles the scope, like its checkbox; a drag past the
    // movement threshold reorders instead and never reports the click.
    const ImVec2 rowSize(cols.width - cols.nameX, ImGui::GetFrameHeight());
    if (ImGui::Selectable(name, false, ImGuiSelectableFlags_NoAutoClosePopups, rowSize)) {
        outcome.chosenScope = ScopeChoice{id, true};
    }
    drawRowKey(id, cols.rightPad);
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload(ScopeRowPayload, &index, sizeof(index));
        ImGui::TextUnformatted(name);
        ImGui::EndDragDropSource();
    }
    ImGui::PopID();
}

void Toolbar::drawRowKey(std::string_view id, float rightPad) const
{
    drawMenuRowAccelerator(m_shortcuts.bindingFor(id).c_str(), rightPad);
}

const char* Toolbar::scopeName(std::string_view id) const
{
    const HostScope* scope = m_registry.byId(id);

    return scope != nullptr ? scopeDisplayName(*scope) : "";
}

PaneRenderOutcome Toolbar::drawRegionToolIcons(bool regionSelected)
{
    PaneRenderOutcome outcome;
    char tooltip[96];
    std::snprintf(tooltip, sizeof(tooltip), "Draw a region (%s)", m_shortcuts.bindings().drawRegion.c_str());
    const int iconPx = iconPixelSize();
    placeRegionToolbox();
    if (iconButton("##draw-region", m_icons.textureId(Icon::Pencil, iconPx), tooltip)) {
        m_picker.request(RegionPickerMode::DrawGlobal);
    }
    ImGui::SameLine(0.0f, 2.0f);
    std::snprintf(tooltip, sizeof(tooltip), "Attach to a window (%s) - click the window or draw inside it",
                  m_shortcuts.bindings().attachWindow.c_str());
    if (iconButton("##attach-window", m_icons.textureId(Icon::SquarePen, iconPx), tooltip)) {
        m_picker.request(RegionPickerMode::AttachWindow);
    }
    ImGui::SameLine(0.0f, 2.0f);
    // The face tool sits last among the region tools, before the reset. It
    // is always available where the platform detects faces: whether any
    // face is on screen is the picker overlay's answer to give, not the
    // toolbar's.
    if (supportsFaceDetection()) {
        std::snprintf(tooltip, sizeof(tooltip), "Attach to a face (%s)", m_shortcuts.bindings().attachFace.c_str());
        if (iconButton("##attach-face", m_icons.textureId(Icon::User, iconPx), tooltip)) {
            m_picker.request(RegionPickerMode::AttachFace);
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    // Last among the region tools: the three before it choose what the scopes
    // read, and this one takes that choice away again.
    std::snprintf(tooltip, sizeof(tooltip), "Clear the region (%s)%s",
                  shortcutLabel(m_shortcuts.bindings().clearRegion).c_str(),
                  regionSelected ? "" : " - no region selected");
    if (iconButton("##clear-region", m_icons.textureId(Icon::SquareDashed, iconPx), tooltip, !regionSelected) &&
        regionSelected) {
        outcome.clearRegion = true;
    }
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::NewLine();

    return outcome;
}

}  // namespace sidescopes
