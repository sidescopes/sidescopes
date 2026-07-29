#include "app/menu_rows.h"

#include <algorithm>
#include <cmath>

#include "app/imgui_ui.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace sidescopes {
namespace {

// How many of each the style push leaves behind, so the pop cannot disagree.
constexpr int PushedVars = 3;
constexpr int PushedColors = 4;

// How much of the accent the chosen row's band carries. Enough that the row
// reads as chosen at a glance, and see-through enough that a chosen row under
// the pointer is brighter again - so the two states are told apart rather than
// one hiding the other.
constexpr float ChosenBandOpacity = 0.55f;

// How strongly a row's leading icon is drawn while its row is neither under the
// pointer nor the chosen one. Derived rather than picked: the key hint lands at
// 0.50 disabled text times AcceleratorOpacity, and the icon is drawn from the
// 0.86 body text, so this is what puts a resting glyph at the same ink weight
// as the numeral across the row from it. A glyph carries more weight than a
// numeral at equal alpha, so matching the ink is what makes a row read
// name-first rather than icon-first.
constexpr float RestingIconOpacity = 0.45f;

// How strongly the key hint is drawn, on top of the theme's already-dim
// disabled text. It states which key reaches a row; it is not a control, and
// it shares the row's right edge with one that is.
constexpr float AcceleratorOpacity = 0.75f;

// A row's highlight band.
struct RowBand
{
    ImVec2 min;
    ImVec2 max;
};

// The band a whole row highlights across: a little above and below the row,
// and nearly the popup's full width - just shy of the border, past the content
// into the window padding - for a native-menu-style highlight edge to edge.
RowBand rowBand(float rowTopY)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = style.ItemSpacing.y * 0.3f;
    const float inset = style.WindowPadding.x * 0.4f;
    const ImVec2 windowPos = ImGui::GetWindowPos();

    return RowBand{ImVec2(windowPos.x + inset, rowTopY - pad),
                   ImVec2(windowPos.x + ImGui::GetWindowSize().x - inset, rowTopY + ImGui::GetFrameHeight() + pad)};
}

// The box a Selectable laid its own LABEL out in, recovered from its item rect.
//
// ImGui deliberately grows a Selectable's item rect beyond that box - half the
// item spacing above and to the left, the remainder below and to the right - so
// a list of them packs together with no dead gap between rows to click in. The
// label is centred in the UNGROWN box. Anything else the row draws against the
// item rect therefore sits low and wide of the name beside it, by half a
// spacing on each axis, which is what the key hint did.
RowBand selectableLabelBox()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    // Truncated, not rounded: this mirrors ImGui's own IM_TRUNC in the growth
    // it is undoing, and half a pixel of disagreement here would put the key
    // back off the name's line.
    const float grownLeft = std::trunc(style.ItemSpacing.x * 0.5f);
    const float grownTop = std::trunc(style.ItemSpacing.y * 0.5f);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();

    return RowBand{ImVec2(min.x + grownLeft, min.y + grownTop),
                   ImVec2(max.x - (style.ItemSpacing.x - grownLeft), max.y - (style.ItemSpacing.y - grownTop))};
}

// Where a key of @p keySize sits over the row just laid down: right-aligned
// @p rightPad in from that row's own edge, on the centre line its name is on.
ImVec2 acceleratorOrigin(const ImVec2& keySize, float rightPad)
{
    const RowBand label = selectableLabelBox();

    return ImVec2(label.max.x - rightPad - keySize.x, label.min.y + (label.max.y - label.min.y - keySize.y) * 0.5f);
}

// Draws the drop insertion line across the open popup at height @p y: a thin
// rule in the gap the dragged row would land in, in a quiet near-white rather
// than the loud drag-drop accent.
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

// Reserves the strip under the last row of a list, so it ends with the gap it
// would keep before a next row - which is where a drop after everything is
// aimed. An empty item, whose own trailing spacing IS the strip: it carries the
// window's content down by one row gap and draws nothing.
// @return Where the rows themselves ended, before the strip.
float reserveDropStrip()
{
    const float rowsBottom = ImGui::GetCursorScreenPos().y;
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    return rowsBottom;
}

// Lays the invisible catch over the rows already drawn, from @p listTop down to
// @p rowsBottomY.
//
// IT MUST BE THE LAST THING SUBMITTED. Dear ImGui's drop target is whatever
// item came last, so anything after this - even an empty item - is what the
// following BeginDragDropTarget() offers to the drop, and a release over the
// list then changes nothing at all. That shipped.
//
// It CLAIMS NO SPACE, so the popup is not a shade wider while a drag is over
// it: it spans exactly what the rows have already made the list. Sized from the
// row width instead it overhangs them by a window padding, because the rows are
// placed a padding in from the edge that width is measured from. That shipped
// too.
//
// The cursor is put back by SUBMITTING this rather than by being moved after
// it. A bare cursor move with nothing following is what ImGui refuses: it has
// been asked to grow the window and given nothing to grow around, and it says
// so in a red error window over the popup, in release builds as much as local
// ones, since the tooltip is only compiled out by IMGUI_DISABLE_DEBUG_TOOLS.
// The strip standing already is what makes that possible - a catch reaching
// exactly @p rowsBottomY lands the cursor back where the list left it.
void layDropCatch(const char* id, const ImVec2& listTop, float rowsBottomY)
{
    const float claimedRight = ImGui::GetCurrentWindow()->DC.CursorMaxPos.x;
    // From the window's own content top, not the first row's edge. The
    // insertion line stands at the top slot for any height above the list, so
    // the band of window padding above the first row is somewhere a drop is
    // being promised: begun at the row instead, a release in that band landed
    // on nothing and the drag snapped back. A release inside the top row's
    // upper half worked, so the failure was a few pixels of aim - the kind a
    // real hand supplies and a test aimed at a row never does.
    const float top = std::min(listTop.y, ImGui::GetCurrentWindow()->InnerRect.Min.y);
    const float bottom = std::max(top, rowsBottomY);
    ImGui::SetCursorScreenPos(ImVec2(listTop.x, top));
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, claimedRight - listTop.x), std::max(1.0f, bottom - top)));
}

}  // namespace

float renameFieldX(float nameX)
{
    return nameX - ImGui::GetStyle().FramePadding.x;
}

float renameFieldWidth(float nameWidth)
{
    return nameWidth + ImGui::GetStyle().FramePadding.x;
}

float menuRowLeadingGap()
{
    return ImGui::GetFontSize();
}

float menuRowNameX(float leadingWidth)
{
    return leadingWidth + menuRowLeadingGap();
}

float menuRowKeyRightPad()
{
    return ImGui::GetFontSize() * 0.75f;
}

void pushMenuRowStyle()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 frameBg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    // The frame padding is pulled right down so a checkbox sits near the text
    // height rather than dwarfing its label.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, ImGui::GetFontSize() * 0.05f));
    // Row-tall Selectables carry their label centred, so the whole row is one
    // target and one drag handle, not just the text line.
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    // Roomier rows than the theme default, so short rows are not condensed and
    // the hover band has space to breathe.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, ImGui::GetFontSize() * 0.7f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clear);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frameBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frameBg);
}

void popMenuRowStyle()
{
    ImGui::PopStyleColor(PushedColors);
    ImGui::PopStyleVar(PushedVars);
}

bool menuRowHovered(float rowTopY)
{
    const RowBand band = rowBand(rowTopY);

    return ImGui::IsMouseHoveringRect(band.min, band.max);
}

void drawMenuRowHover(float rowTopY)
{
    if (ImGui::GetDragDropPayload() != nullptr || !menuRowHovered(rowTopY)) {
        return;
    }
    const RowBand band = rowBand(rowTopY);
    ImGui::GetWindowDrawList()->AddRectFilled(band.min, band.max, ImGui::GetColorU32(ImGuiCol_ButtonHovered),
                                              ImGui::GetStyle().FrameRounding);
}

void drawMenuRowChosen(float rowTopY)
{
    const RowBand band = rowBand(rowTopY);
    ImGui::GetWindowDrawList()->AddRectFilled(band.min, band.max,
                                              ImGui::GetColorU32(ImGuiCol_ButtonActive, ChosenBandOpacity),
                                              ImGui::GetStyle().FrameRounding);
}

float menuRowIconWidth()
{
    return ImGui::GetFrameHeight() + ImGui::GetFontSize() * 0.5f;
}

void offerMenuRowDrag(const char* payloadType, int index, const char* label)
{
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        ImGui::SetDragDropPayload(payloadType, &index, sizeof(index));
        ImGui::TextUnformatted(label);
        ImGui::EndDragDropSource();
    }
}

std::optional<MenuRowMove> landMenuRowDrag(const char* payloadType, const ImVec2& listTop, int count)
{
    // Before anything else, and whether or not anything is being dragged: the
    // list is one size at rest and mid-gesture.
    const float rowsBottom = reserveDropStrip();
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    if (count < 1 || drag == nullptr || !drag->IsDataType(payloadType)) {
        return std::nullopt;
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    // The real per-row pitch from the laid-out list, so the line tracks the rows
    // whatever their height rather than a computed guess. Measured to where the
    // ROWS ended, never past the strip.
    const float advance = (rowsBottom - listTop.y) / static_cast<float>(count);
    const int gap = insertionGap(listTop.y, count, advance, spacing);
    layDropCatch("##menu-row-drop", listTop, rowsBottom);
    int from = -1;
    if (ImGui::BeginDragDropTarget()) {
        // The line draws inside the target block, so it stands exactly where a
        // release will land and nowhere else. Drawn unconditionally it kept
        // promising the top slot while the pointer was past the catch
        // entirely - a promise the release then broke.
        drawInsertionBar(insertionBarY(listTop.y, gap, advance, spacing));
        const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(payloadType, ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (payload != nullptr) {
            from = *static_cast<const int*>(payload->Data);
        }
        ImGui::EndDragDropTarget();
    }
    if (from < 0) {
        return std::nullopt;
    }

    return MenuRowMove{from, gap};
}

bool menuRowIconButton(const char* id, ImTextureID texture, const char* tooltip, bool emphasized)
{
    // Sized to the row rather than to the toolbar: iconButton's box is a fixed
    // amount taller than a line of text, which in a popup whose frame padding
    // is pulled right down would make the rows carrying one visibly taller
    // than the rest. The GLYPH keeps the toolbar's size, so it reads as the
    // same icon at the same weight.
    //
    // THE GLYPH IS ALWAYS DRAWN; only its strength answers to @p emphasized.
    // Hiding it outright and holding its box open - which is what a row action
    // that appears on hover asks for - leaves a gutter of nothing down the
    // whole list, and NOT holding the box open steps every name sideways as
    // the pointer moves. Dimming escapes both: the space is occupied by
    // something meaningful, so there is no gutter and nothing to shift.
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(menuRowIconWidth(), ImGui::GetFrameHeight()));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), ImGui::GetStyle().FrameRounding);
    }
    const float side = ImGui::GetTextLineHeight();
    const ImVec2 glyph(std::round(min.x + (max.x - min.x - side) / 2.0f),
                       std::round(min.y + (max.y - min.y - side) / 2.0f));
    draw->AddImage(texture, glyph, ImVec2(glyph.x + side, glyph.y + side), ImVec2(0, 0), ImVec2(1, 1),
                   ImGui::GetColorU32(ImGuiCol_Text, emphasized ? 1.0f : RestingIconOpacity));
    wrappedTooltip(tooltip);

    return pressed;
}

void drawMenuRowAccelerator(const char* key, float rightPad)
{
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    const ImVec2 at = acceleratorOrigin(ImGui::CalcTextSize(key), rightPad);
    ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled, AcceleratorOpacity), key);
}

}  // namespace sidescopes
