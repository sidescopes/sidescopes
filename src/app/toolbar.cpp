#include "app/toolbar.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/imgui_ui.h"
#include "app/region_picker.h"
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

// The drag-and-drop payload tag for a dragged scope row: its index in the
// shown list travels as the payload.
constexpr const char* ScopeRowPayload = "ss_scope_row";

// Lifts the scope at @p from and reinserts it at the @p gap slot (0..size,
// counting the gaps between rows), so the dragged row lands where the insertion
// bar showed. Removing @p from shifts every later slot down by one.
void moveScope(std::vector<std::string>& order, int from, int gap)
{
    std::string moved = std::move(order[from]);
    order.erase(order.begin() + from);
    order.insert(order.begin() + (gap > from ? gap - 1 : gap), std::move(moved));
}

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

// Draws one hover highlight behind a whole menu row whose content starts at
// @p rowTopY, so hovering reads as a single row rather than the checkbox and
// the label lighting up apart. The band reaches a little above and below the
// row content, and runs nearly the popup's full width - just shy of the border,
// past the content into the window padding - so a native-menu-style highlight
// covers the row edge to edge. Skipped mid-drag, where the insertion bar is the cue.
void drawRowHover(float rowTopY)
{
    if (ImGui::GetDragDropPayload() != nullptr) {
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = style.ItemSpacing.y * 0.3f;
    const float inset = style.WindowPadding.x * 0.4f;
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 barMin(windowPos.x + inset, rowTopY - pad);
    const ImVec2 barMax(windowPos.x + ImGui::GetWindowSize().x - inset, rowTopY + ImGui::GetFrameHeight() + pad);
    if (!ImGui::IsMouseHoveringRect(barMin, barMax)) {
        return;
    }
    ImGui::GetWindowDrawList()->AddRectFilled(barMin, barMax, ImGui::GetColorU32(ImGuiCol_ButtonHovered),
                                              style.FrameRounding);
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
// rows from @p listTop spanning @p width, a full-list drop catch that lands the
// reorder wherever the release falls: live only during the drag, it overlays the
// rows without disturbing their clicks, which the drag suspends. @return Whether
// the order of @p shown changed.
bool applyScopeDrop(std::vector<std::string>& shown, ImVec2 listTop, float width)
{
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    if (drag == nullptr || !drag->IsDataType(ScopeRowPayload)) {
        return false;
    }
    const int count = static_cast<int>(shown.size());
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
    if (from >= 0 && gap != from && gap != from + 1) {
        moveScope(shown, from, gap);

        return true;
    }

    return false;
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
        if (scope.letter == 0) {
            continue;
        }
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
    // The scopes on screen lead, in pane order, so the menu mirrors the layout
    // and can drag-reorder it; the rest follow below a rule. Every row toggles,
    // never solos, so the menu stays open across several clicks. The frame
    // padding is pulled right down so the checkbox sits near the text height
    // rather than dwarfing its label. One quiet bar highlights each whole row
    // (drawRowHover), so the checkbox and the name Selectable must not light up
    // on their own - both their highlights are hidden, off the theme's loud
    // selection blue too.
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 frameBg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, ImGui::GetFontSize() * 0.05f));
    // Row-tall name Selectables (below) carry their label centred, so the whole
    // row is one drag handle and drop target, not just the text line.
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    // Roomier rows than the theme default, so the short checkbox rows are not
    // condensed and the hover band has space to breathe.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, ImGui::GetFontSize() * 0.7f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frameBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frameBg);
    const ScopeMenuColumns cols = scopeColumns();
    std::vector<std::string> shown = m_view.stack().ids();
    if (shown.size() > 1) {
        ImGui::TextDisabled("drag to reorder");
    }
    if (appendShownScopes(shown, cols, outcome)) {
        outcome.reorderedStack = shown;
        outcome.preferencesSaveDue = true;
    }
    bool ruled = false;
    for (const HostScope& scope : m_registry.scopes()) {
        if (scope.letter == 0 || m_view.stack().shows(scope.id)) {
            continue;
        }
        if (!ruled) {
            ImGui::Separator();
            ruled = true;
        }
        if (drawAddRow(scope.id, cols)) {
            outcome.chosenScope = ScopeChoice{scope.id, true};
        }
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
}

bool Toolbar::appendShownScopes(std::vector<std::string>& shown, const ScopeMenuColumns& cols,
                                PaneRenderOutcome& outcome)
{
    // Each on-screen scope: a checkbox that removes it, then its row-tall name as
    // the drag handle - so a reorder is never read as a remove. The line and the
    // drop are handled once, past the loop, from the whole list's geometry.
    const float selWidth = cols.width - cols.nameX;
    const ImVec2 rowSize(selWidth, ImGui::GetFrameHeight());
    const ImVec2 listTop = ImGui::GetCursorScreenPos();
    for (int n = 0; n < static_cast<int>(shown.size()); ++n) {
        const char* name = scopeName(shown[n]);
        drawRowHover(ImGui::GetCursorScreenPos().y);
        ImGui::PushID(shown[n].c_str());
        bool on = true;
        if (ImGui::Checkbox("##shown", &on)) {
            outcome.chosenScope = ScopeChoice{shown[n], true};
        }
        ImGui::SameLine(cols.nameX);
        // A click on the name removes the scope, like its checkbox; a drag past
        // the movement threshold reorders instead and never reports the click.
        if (ImGui::Selectable(name, false, ImGuiSelectableFlags_NoAutoClosePopups, rowSize)) {
            outcome.chosenScope = ScopeChoice{shown[n], true};
        }
        drawRowKey(shown[n], cols.rightPad);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload(ScopeRowPayload, &n, sizeof(n));
            ImGui::TextUnformatted(name);
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }

    return applyScopeDrop(shown, listTop, cols.width);
}

bool Toolbar::drawAddRow(const std::string& id, const ScopeMenuColumns& cols)
{
    // The same layout as an on-screen row, minus the drag: a checkbox and the
    // full-width name both add the scope when clicked.
    drawRowHover(ImGui::GetCursorScreenPos().y);
    ImGui::PushID(id.c_str());
    bool on = false;
    bool clicked = ImGui::Checkbox("##add", &on);
    ImGui::SameLine(cols.nameX);
    const ImVec2 rowSize(cols.width - cols.nameX, ImGui::GetFrameHeight());
    if (ImGui::Selectable(scopeName(id), false, ImGuiSelectableFlags_NoAutoClosePopups, rowSize)) {
        clicked = true;
    }
    drawRowKey(id, cols.rightPad);
    ImGui::PopID();

    return clicked;
}

void Toolbar::drawRowKey(std::string_view id, float rightPad) const
{
    const std::string key = m_shortcuts.bindingFor(id);
    if (key.empty()) {
        return;
    }
    // Drawn over the row just laid down, right-aligned a margin in from its edge
    // and dimmed, the way a menu shows an accelerator.
    const ImVec2 keySize = ImGui::CalcTextSize(key.c_str());
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const ImVec2 at(rowMax.x - rightPad - keySize.x, rowMin.y + (rowMax.y - rowMin.y - keySize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), key.c_str());
}

const char* Toolbar::scopeName(std::string_view id) const
{
    const HostScope* scope = m_registry.byId(id);

    return scope != nullptr ? scopeDisplayName(*scope) : "";
}

void Toolbar::placeRegionToolbox() const
{
    // The brief note after an attached window closed out from under its region
    // stays on the left, by the scopes cluster, clear of the toolbox.
    if (glfwGetTime() < m_attachNoticeUntil) {
        ImGui::TextDisabled("%s", m_attachNotice.c_str());
        ImGui::SameLine(0.0f, 8.0f);
    }
    // The region toolbox is a constant-width cluster: state dims a tool, it
    // never removes one, so the row reflows only when the WINDOW changes -
    // not when the scope stack does. Right-aligned while it shares the row
    // with the scopes; flush left when it wraps to a row of its own. Narrow
    // windows are the tall beside-the-editor shape, which has the height
    // for a second row; wide strips keep one row.
    const int iconCount = 3 + (supportsFaceDetection() ? 1 : 0);
    const float chip = ImGui::GetTextLineHeight() + 12.0f;
    const float width = static_cast<float>(iconCount) * chip + static_cast<float>(iconCount - 1) * 2.0f;
    const float right = ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetCursorPosX() + width + 8.0f > right) {
        ImGui::NewLine();
    } else {
        ImGui::SetCursorPosX(right - width);
    }
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

void Toolbar::showAttachNotice(std::string message)
{
    m_attachNotice = std::move(message);
    m_attachNoticeUntil = glfwGetTime() + 5.0;
}

}  // namespace sidescopes
