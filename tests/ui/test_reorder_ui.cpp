// Headless asserts on the WHOLE drag-to-reorder gesture over a menu row list -
// press on a row, move to another, release - driven through the real
// src/app/menu_rows.cpp pair that ships: offerMenuRowDrag on the row and
// landMenuRowDrag under the list.
//
// WHY THIS SUITE IS SEPARATE, and it is the point of it. Three defects have
// shipped in this one gesture, and every one of them got past a test that
// asserted a PART of it: that the model moved a row, that the catch reached far
// enough, that the toolkit was left nothing pending. Each was true while the
// gesture as a whole did nothing. So the tests here drive the mouse and assert
// the ORDER THAT COMES OUT - the only assertion the user's complaint is about.
// A test that stops short of the release belongs somewhere else.
//
// WHAT IT DOES NOT COVER, stated rather than left to be discovered: that the
// scope selector CALLS this pair. Constructing a Toolbar needs a graphics
// backend and a region picker, neither of which exists headlessly, so the list
// here is the shipped gesture drawn against a stand-in list rather than the
// shipped popup. Every defect so far has been in the gesture, not the wiring.
//
// Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO, used under
// its Free License; fetched at build time, never vendored.

#include <string>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "app/menu_rows.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "ui_test_harness.h"

namespace sidescopes {
namespace {

/// The payload tag the harness list drags under, standing in for the scope
/// menu's own.
constexpr const char* RowPayload = "ss_test_row";

/// How wide the harness rows are, past the leading control.
constexpr float RowNameWidth = 160.0f;

/// What one frame of the harness list laid down, and what the gesture did to
/// it. A function-local static keeps it reachable from the engine's captureless
/// GuiFunc and TestFunc.
struct RowList
{
    std::vector<std::string> order{"one", "two", "three", "four"};
    /// Each row's own box, in screen coordinates, from the frame just drawn -
    /// not the item rect, which ImGui grows past the row on both sides.
    std::vector<ImRect> rowBoxes;
    /// The strip under the last row, where a drop after everything is aimed.
    float rowsBottom = 0.0f;
    float lastRowBottom = 0.0f;
    /// What the window's content measured, with and without a drag in flight.
    ImVec2 contentAtRest{0.0f, 0.0f};
    ImVec2 contentWhileDragging{0.0f, 0.0f};
    /// Whether a bare cursor move was left pending - the toolkit-error state.
    bool cursorMovePending = false;
    /// How many frames a drag was in flight, so a test can prove the gesture
    /// really happened rather than passing because nothing did.
    int dragFrames = 0;
    /// How many drops landed, and the last one's slot.
    int drops = 0;
    int lastGap = -1;
};

RowList& rows()
{
    static RowList instance;

    return instance;
}

/// The list, drawn the shape the scope menu draws it: a leading control, the
/// name as the drag handle, and the shared reorder underneath.
void rowListGui(ImGuiTestContext*)
{
    RowList& list = rows();
    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Always);
    ImGui::Begin("Rows", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    pushMenuRowStyle();
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    const float nameX = menuRowNameX(ImGui::GetFrameHeight());
    const ImVec2 listTop = ImGui::GetCursorScreenPos();
    const int count = static_cast<int>(list.order.size());
    list.rowBoxes.clear();
    for (int n = 0; n < count; ++n) {
        const char* name = list.order[static_cast<std::size_t>(n)].c_str();
        const ImVec2 rowTop = ImGui::GetCursorScreenPos();
        const float rowHeight = ImGui::GetFrameHeight();
        ImGui::PushID(n);
        bool on = true;
        ImGui::Checkbox("##shown", &on);
        ImGui::SameLine(nameX);
        ImGui::Selectable(name, false, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(RowNameWidth, rowHeight));
        offerMenuRowDrag(RowPayload, n, name);
        ImGui::PopID();
        list.rowBoxes.push_back(ImRect(rowTop.x, rowTop.y, ImGui::GetItemRectMax().x, rowTop.y + rowHeight));
        list.lastRowBottom = rowTop.y + rowHeight;
    }

    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    const bool dragging = drag != nullptr && drag->IsDataType(RowPayload);
    list.rowsBottom = ImGui::GetCursorScreenPos().y;
    if (const auto moved = landMenuRowDrag(RowPayload, listTop, count)) {
        list.drops++;
        list.lastGap = moved->gap;
        // The move ScopeOrder::move makes, so the assertion is about the order
        // the user ends up looking at.
        if (moved->gap != moved->from && moved->gap != moved->from + 1) {
            const std::string lifted = list.order[static_cast<std::size_t>(moved->from)];
            list.order.erase(list.order.begin() + moved->from);
            list.order.insert(list.order.begin() + (moved->gap > moved->from ? moved->gap - 1 : moved->gap), lifted);
        }
    }

    const ImVec2 content(window->DC.CursorMaxPos.x - window->DC.CursorStartPos.x,
                         window->DC.CursorMaxPos.y - window->DC.CursorStartPos.y);
    if (dragging) {
        list.dragFrames++;
        list.contentWhileDragging = content;
    } else {
        list.contentAtRest = content;
    }
    list.cursorMovePending = window->DC.IsSetPos;

    popMenuRowStyle();
    ImGui::End();
}

/// Resets the list and lets a frame lay it out, so every test starts from the
/// same order and reads this frame's geometry rather than the last test's.
void freshList(ImGuiTestContext* ctx)
{
    RowList& list = rows();
    list.order = {"one", "two", "three", "four"};
    list.drops = 0;
    list.dragFrames = 0;
    list.lastGap = -1;
    ctx->SetRef("Rows");
    ctx->Yield(3);
}

/// A point @p part of the way down row @p index, where 0 is its top edge and 1
/// its bottom. Which HALF a release falls in is what decides the slot - above
/// the middle inserts before the row, below it after - so a test aims at a
/// quarter or a three-quarter point and never at the boundary itself.
ImVec2 withinRow(int index, float part)
{
    const ImRect& box = rows().rowBoxes[static_cast<std::size_t>(index)];

    return ImVec2((box.Min.x + box.Max.x) * 0.5f, box.Min.y + (box.Max.y - box.Min.y) * part);
}

/// Presses on @p from's row, moves to @p to, and lets go.
void dragRowTo(ImGuiTestContext* ctx, int from, ImVec2 to)
{
    ctx->MouseTeleportToPos(withinRow(from, 0.5f));
    ctx->MouseDown(0);
    ctx->MouseMoveToPos(to);
    ctx->Yield(2);
    ctx->MouseUp(0);
    ctx->Yield(2);
}

/// SYMPTOM IF BROKEN: dragging a scope in the selector does nothing. The row
/// lifts, the popup reacts, and releasing it changes no order at all.
///
/// This is the whole gesture and the only test that has ever asserted it. It
/// last broke because an empty item was submitted AFTER the drop catch to put
/// the cursor back: Dear ImGui offers the LAST item to a drop, so the release
/// was tested against that empty item, missed it every time, and the model - a
/// move that has always worked - was simply never asked.
void aRowDraggedOverAnotherChangesTheOrder(ImGuiTestContext* ctx)
{
    freshList(ctx);
    RowList& list = rows();
    IM_CHECK_STR_EQ(list.order[0].c_str(), "one");

    // Down the list: the first row, released over the lower half of the third,
    // which is the gap after it.
    dragRowTo(ctx, 0, withinRow(2, 0.75f));

    // The drag really happened, so a pass cannot come from a gesture that never
    // started.
    IM_CHECK_GT(list.dragFrames, 0);
    IM_CHECK_EQ(list.drops, 1);
    IM_CHECK_EQ(list.lastGap, 3);
    IM_CHECK_STR_EQ(list.order[0].c_str(), "two");
    IM_CHECK_STR_EQ(list.order[1].c_str(), "three");
    IM_CHECK_STR_EQ(list.order[2].c_str(), "one");
    IM_CHECK_STR_EQ(list.order[3].c_str(), "four");
}

/// SYMPTOM IF BROKEN: a row can be moved down the list but not back up.
///
/// The gap arithmetic is not symmetric - removing the lifted row shifts every
/// later slot - so the upward drag is a different path through it and gets its
/// own assertion rather than being assumed from the downward one.
void aRowDraggedUpwardChangesTheOrder(ImGuiTestContext* ctx)
{
    freshList(ctx);
    RowList& list = rows();

    // The last row, released over the upper half of the first, which is the gap
    // before everything.
    dragRowTo(ctx, 3, withinRow(0, 0.25f));

    IM_CHECK_GT(list.dragFrames, 0);
    IM_CHECK_EQ(list.drops, 1);
    IM_CHECK_EQ(list.lastGap, 0);
    IM_CHECK_STR_EQ(list.order[0].c_str(), "four");
    IM_CHECK_STR_EQ(list.order[1].c_str(), "one");
    IM_CHECK_STR_EQ(list.order[2].c_str(), "two");
    IM_CHECK_STR_EQ(list.order[3].c_str(), "three");
}

/// SYMPTOM IF BROKEN: a scope cannot be dragged to the END of the list. Every
/// other position takes the drop and the last one silently refuses.
///
/// Every drop position but the last sits BETWEEN two rows. The last - after
/// everything - can only be aimed at below the final row, so the strip under it
/// IS that position: released there, the drop has to land in the last slot, not
/// be swallowed and not be rounded back to the second-to-last.
void aRowDraggedPastTheLastRowLandsAtTheEnd(ImGuiTestContext* ctx)
{
    freshList(ctx);
    RowList& list = rows();

    // Below the last row's own edge, in the strip the list reserves - which is
    // where the insertion line stands for this position and therefore where a
    // user aims. The strip has to be there at all, first.
    IM_CHECK_GT(list.rowsBottom, list.lastRowBottom);
    const float belowEverything = (list.lastRowBottom + list.rowsBottom) * 0.5f;
    dragRowTo(ctx, 0, ImVec2(withinRow(0, 0.5f).x, belowEverything));

    IM_CHECK_GT(list.dragFrames, 0);
    IM_CHECK_EQ(list.drops, 1);
    IM_CHECK_EQ(list.lastGap, static_cast<int>(list.order.size()));
    IM_CHECK_STR_EQ(list.order[0].c_str(), "two");
    IM_CHECK_STR_EQ(list.order[1].c_str(), "three");
    IM_CHECK_STR_EQ(list.order[2].c_str(), "four");
    IM_CHECK_STR_EQ(list.order[3].c_str(), "one");
}

/// SYMPTOM IF BROKEN: the drop-down grows taller and wider the moment a drag
/// starts, and shrinks back when it ends - the list changing size under the
/// pointer at the exact moment the user is aiming at a place in it.
///
/// Both halves were real. The height came from the end-of-list strip appearing
/// only while a drag was in flight; it is reserved always now, so the list is a
/// few pixels taller at rest and never moves. The width came from the catch
/// being laid at the content's left edge but sized from the WINDOW's, so it
/// overhung the rows by one window padding and the popup grew to fit it.
void theListIsOneSizeDraggedOrNot(ImGuiTestContext* ctx)
{
    freshList(ctx);
    RowList& list = rows();
    const ImVec2 rest = list.contentAtRest;
    IM_CHECK_GT(rest.x, 0.0f);
    IM_CHECK_GT(rest.y, 0.0f);

    ctx->MouseTeleportToPos(withinRow(0, 0.5f));
    ctx->MouseDown(0);
    ctx->MouseMoveToPos(withinRow(2, 0.75f));
    ctx->Yield(2);
    IM_CHECK_GT(list.dragFrames, 0);
    const ImVec2 dragged = list.contentWhileDragging;
    ctx->MouseUp(0);
    ctx->Yield(2);

    IM_CHECK_EQ(dragged.x, rest.x);
    IM_CHECK_EQ(dragged.y, rest.y);
}

/// SYMPTOM IF BROKEN: dragging a scope raises Dear ImGui's own error window
/// over the popup - "Code uses SetCursorPos()/SetCursorScreenPos() to extend
/// window/parent boundaries" - and the popup mis-sizes underneath it.
///
/// Overlaying a catch on rows already drawn means moving the cursor back up the
/// list. Moving it DOWN again afterwards is a bare cursor move with nothing
/// submitted after it, which is exactly what ImGui refuses. Nothing may be
/// submitted after the catch either, because the drop target is the last item -
/// so the cursor has to be restored by the catch's OWN extent, which is what
/// reserving the strip first makes possible.
///
/// The error window is not confined to local builds: the tooltip it comes from
/// is compiled out only by IMGUI_DISABLE_DEBUG_TOOLS, which this project does
/// not define, so a release binary showed it too.
void draggingLeavesTheToolkitNothingPending(ImGuiTestContext* ctx)
{
    freshList(ctx);
    RowList& list = rows();
    IM_CHECK_EQ(list.cursorMovePending, false);

    ctx->MouseTeleportToPos(withinRow(0, 0.5f));
    ctx->MouseDown(0);
    ctx->MouseMoveToPos(withinRow(2, 0.75f));
    ctx->Yield(2);
    IM_CHECK_GT(list.dragFrames, 0);
    // Mid-drag is when the catch is laid, so it is mid-drag that this matters.
    IM_CHECK_EQ(list.cursorMovePending, false);
    ctx->MouseUp(0);
    ctx->Yield(2);
    IM_CHECK_EQ(list.cursorMovePending, false);
}

void registerReorderTests(ImGuiTestEngine* engine)
{
    ImGuiTest* down = IM_REGISTER_TEST(engine, "reorder", "a_drag_changes_the_order");
    down->GuiFunc = rowListGui;
    down->TestFunc = aRowDraggedOverAnotherChangesTheOrder;

    ImGuiTest* up = IM_REGISTER_TEST(engine, "reorder", "a_drag_upward_changes_the_order");
    up->GuiFunc = rowListGui;
    up->TestFunc = aRowDraggedUpwardChangesTheOrder;

    ImGuiTest* last = IM_REGISTER_TEST(engine, "reorder", "a_drag_past_the_last_row_lands_at_the_end");
    last->GuiFunc = rowListGui;
    last->TestFunc = aRowDraggedPastTheLastRowLandsAtTheEnd;

    ImGuiTest* size = IM_REGISTER_TEST(engine, "reorder", "the_list_is_one_size_dragged_or_not");
    size->GuiFunc = rowListGui;
    size->TestFunc = theListIsOneSizeDraggedOrNot;

    ImGuiTest* pending = IM_REGISTER_TEST(engine, "reorder", "dragging_leaves_nothing_pending");
    pending->GuiFunc = rowListGui;
    pending->TestFunc = draggingLeavesTheToolkitNothingPending;
}

}  // namespace
}  // namespace sidescopes

int main()
{
    using namespace sidescopes;

    return uitest::runSuite("reorder", registerReorderTests, /*expectedTests=*/5);
}
