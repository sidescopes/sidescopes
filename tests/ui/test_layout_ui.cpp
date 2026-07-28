// Headless asserts on the seating rules a row of mixed elements obeys - the
// arithmetic in src/app/row_layout.h that decides where an icon, a swatch and a
// line of text sit relative to one another.
//
// These are invariants, not goldens: they hold whatever font the platform
// loads, so the SAME asserts pass on macOS with SF and on Windows with Segoe.
// That is the point - the two OSes cannot be compared pixel for pixel with
// native fonts, but they can be held to identical alignment rules.
//
// Each test here encodes a defect that shipped. When one fails, read the
// comment above it: it says which visible symptom the rule prevents.
//
// Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO, used under
// its Free License; fetched at build time, never vendored.

#include <algorithm>
#include <cmath>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "app/interface_style.h"
#include "app/pane_note.h"
#include "app/row_layout.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "ui_test_harness.h"

namespace sidescopes {
namespace {

/// Box origins a button can land on, including the fractional ones a window
/// dragged to an odd position produces.
const std::vector<float> ProbeOrigins = {0.0f, 1.0f, 17.0f, 100.0f, 289.0f, 0.25f, 0.5f, 100.5f, 317.5f};

/// SYMPTOM IF BROKEN: icons render soft, and shift by a pixel against the text
/// beside them as the window moves across the screen.
///
/// A glyph must land on whole pixels. The box stands an odd number of pixels
/// taller than the glyph, so its centre falls on a half pixel; seating the
/// glyph by that centre put it on half pixels too - blurry, and rounded
/// differently depending on where the window sat.
void glyphSeatsOnWholePixels(ImGuiTestContext*)
{
    const float side = ImGui::GetTextLineHeight();
    const float width = iconButtonWidth();
    const float height = iconButtonHeight();
    for (const float origin : ProbeOrigins) {
        const ImVec2 min(origin, origin);
        const ImVec2 max(origin + width, origin + height);
        const ImVec2 glyph = iconGlyphOrigin(min, max, side);
        IM_CHECK_EQ(glyph.x, std::floor(glyph.x));
        IM_CHECK_EQ(glyph.y, std::floor(glyph.y));

        // ...and still centred, to within the half pixel that rounding costs.
        const float centreError = std::fabs((glyph.y + side / 2.0f) - (min.y + height / 2.0f));
        IM_CHECK_LE(centreError, 0.5f);
    }
}

/// SYMPTOM IF BROKEN: the pin tool's icon sits a shade higher or lower than the
/// channel letters and swatch sharing its row.
///
/// Text and swatches drop by rowTextDrop() to meet the icon's taller box; the
/// icon's own glyph must land at exactly that same offset, or the row has two
/// centre lines instead of one.
void rowElementsShareOneCentreLine(ImGuiTestContext*)
{
    const float side = ImGui::GetTextLineHeight();
    const ImVec2 min(0.0f, 0.0f);
    const ImVec2 max(iconButtonWidth(), iconButtonHeight());
    const ImVec2 glyph = iconGlyphOrigin(min, max, side);
    IM_CHECK_EQ(glyph.y - min.y, rowTextDrop());
}

/// SYMPTOM IF BROKEN: a reading looks bound to the wrong channel - "R 99% G"
/// reads as though the 99% belongs to the G on its right.
///
/// Every group is a letter, one gap, then its value. What separates one group
/// from the next must beat that inner gap, or the eye groups across the wrong
/// boundary. A short value widens the separation and never the inner gap,
/// because values are left-aligned at fixed positions.
void readoutColumnsBindValuesToTheirOwnLabel(ImGuiTestContext*)
{
    const ReadoutColumns columns = measureReadoutColumns();
    const float group = columns.label + columns.gap + ImGui::CalcTextSize("100%").x;
    const float separation = columns.stride - group;
    IM_CHECK_GT(separation, columns.gap);
    IM_CHECK_EQ(columns.width, 2.0f * columns.stride + group);

    // The widest reading still clears its neighbour's letter.
    IM_CHECK_GT(columns.stride, group);
}

/// SYMPTOM IF BROKEN: the status strip's contents sit visibly high or low in
/// the gap under the panes.
///
/// The strip is bounded above by the item spacing and below by the window
/// padding; the row takes up half their difference so it lands centred in what
/// the eye actually sees. It must never push upward into the panes.
void statusRowSitsCentredInItsStrip(ImGuiTestContext*)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float expected = std::max(0.0f, std::round((style.WindowPadding.y - style.ItemSpacing.y) / 2.0f));
    IM_CHECK_EQ(statusRowOffset(), expected);
    IM_CHECK_GE(statusRowOffset(), 0.0f);
}

// Two rows built the two ways a status row can be filled. Their probe items'
// offsets from their own row top are recorded for the test to compare.
struct RowProbe
{
    float toolFirst = -1.0f;
    float textFirst = -1.0f;
};

RowProbe& rowProbe()
{
    static RowProbe instance;

    return instance;
}

/// Draws the row twice: once opening with the tall tool, once opening with a
/// dropped line of text, both behind the full-height anchor the real bar uses.
void rowOriginGui(ImGuiTestContext*)
{
    ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f), ImGuiCond_Always);
    ImGui::Begin("Rows", nullptr, ImGuiWindowFlags_NoSavedSettings);

    const auto probeRow = [](const char* id, bool toolFirst) {
        ImGui::Dummy(ImVec2(0.0f, iconButtonHeight()));
        const float rowTop = ImGui::GetItemRectMin().y;
        ImGui::SameLine(0.0f, 0.0f);
        if (toolFirst) {
            ImGui::InvisibleButton(id, ImVec2(iconButtonWidth(), iconButtonHeight()));
        } else {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop());
            ImGui::TextUnformatted("message");
        }
        ImGui::SameLine(200.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop());
        ImGui::TextUnformatted("R 99%");

        return ImGui::GetItemRectMin().y - rowTop;
    };

    rowProbe().toolFirst = probeRow("##tool", true);
    rowProbe().textFirst = probeRow("##text", false);

    ImGui::End();
}

/// SYMPTOM IF BROKEN: the live swatch jumps up and down as status messages come
/// and go.
///
/// A helper that moves the cursor before drawing moves the whole LINE's origin
/// when it is the row's first item, not just itself - so a row opening with a
/// dropped message seated everything after it lower than a row opening with the
/// tool. The full-height anchor fixes the origin before anything stands on it.
void rowOriginIgnoresWhichElementComesFirst(ImGuiTestContext* ctx)
{
    ctx->SetRef("Rows");
    ctx->Yield();
    IM_CHECK_GE(rowProbe().toolFirst, 0.0f);
    IM_CHECK_EQ(rowProbe().toolFirst, rowProbe().textFirst);
}

/// SYMPTOM IF BROKEN: a row seats correctly at 100% but an icon or a line of
/// text drifts off the shared centre line at another interface scale - the
/// "fine on my display, wrong on a HiDPI one" class the interface-scale bug was.
///
/// The seating rules are ratios, not pixel constants, so they must hold at every
/// scale the UI Scaling menu offers. Each scale rebuilds the style and font; a
/// frame applies them, the font-height guard proves the scale really took (so a
/// no-op would not make the rest vacuous), then the same invariants are checked.
void seatingHoldsAtEveryScale(ImGuiTestContext* ctx)
{
    const ImGuiStyle saved = ImGui::GetStyle();

    applyInterfaceScale(1.0f);
    ctx->Yield();
    const float baseLine = ImGui::GetTextLineHeight();

    for (const float scale : {0.5f, 1.5f, 2.0f}) {
        applyInterfaceScale(scale);
        ctx->Yield();

        // The scaled font actually took effect; without this the checks below
        // would pass trivially at the unscaled size.
        IM_CHECK_LE(std::fabs(ImGui::GetTextLineHeight() - baseLine * scale), 1.0f);

        const float side = ImGui::GetTextLineHeight();
        const ImVec2 min(0.0f, 0.0f);
        const ImVec2 max(iconButtonWidth(), iconButtonHeight());
        const ImVec2 glyph = iconGlyphOrigin(min, max, side);
        IM_CHECK_EQ(glyph.x, std::floor(glyph.x));
        IM_CHECK_EQ(glyph.y, std::floor(glyph.y));
        IM_CHECK_EQ(glyph.y - min.y, rowTextDrop());
    }

    ImGui::GetStyle() = saved;
    ctx->Yield();
}

/// SYMPTOM IF BROKEN: the note telling the user how to fill an empty scope is
/// cut off by the edge of a pane dragged narrow.
///
/// The note gives way rather than being clipped, so the rule is stated at the
/// exact width where it stops fitting, from both sides of it.
void thePaneNoteGivesWayBeforeItIsCut(ImGuiTestContext*)
{
    constexpr float Note = 120.0f;
    const float exact = Note + 2.0f * RowSeparation;

    IM_CHECK(paneNoteFits(Note, exact));
    IM_CHECK(paneNoteFits(Note, exact + 1.0f));
    IM_CHECK(!paneNoteFits(Note, exact - 1.0f));

    // It keeps a margin at both edges, not one: a pane exactly as wide as the
    // words drops them.
    IM_CHECK(!paneNoteFits(Note, Note));
}

/// What one call to drawPaneNote left in the draw list: how many vertices, and
/// the box the glyph quads spanned.
struct DrawnNote
{
    int vertices = 0;
    ImVec2 min{0.0f, 0.0f};
    ImVec2 max{0.0f, 0.0f};
};

/// Draws the note into a pane of @p paneSize at the window's cursor and reports
/// what reached the toolkit. Kept well inside the window: Dear ImGui culls TEXT
/// against the clip rectangle, so a pane at arbitrary screen coordinates would
/// drop every glyph and leave this blind.
DrawnNote drawnNote(const ImVec2& paneSize)
{
    const ImDrawList* draw = ImGui::GetWindowDrawList();
    const int before = draw->VtxBuffer.Size;
    drawPaneNote(ImGui::GetCursorScreenPos(), paneSize, "Draw a region (D)");

    DrawnNote drawn;
    drawn.vertices = draw->VtxBuffer.Size - before;
    if (drawn.vertices == 0) {
        return drawn;
    }
    drawn.min = draw->VtxBuffer[before].pos;
    drawn.max = drawn.min;
    for (int vertex = before; vertex < draw->VtxBuffer.Size; ++vertex) {
        const ImVec2 at = draw->VtxBuffer[vertex].pos;
        drawn.min = ImVec2(std::min(drawn.min.x, at.x), std::min(drawn.min.y, at.y));
        drawn.max = ImVec2(std::max(drawn.max.x, at.x), std::max(drawn.max.y, at.y));
    }

    return drawn;
}

// The suite's readings, taken in the GuiFunc where a draw list exists and
// asserted in the TestFunc. A function-local static keeps them reachable from
// the engine's captureless callbacks.
struct NoteReadings
{
    DrawnNote small;
    DrawnNote wider;
    DrawnNote taller;
    DrawnNote narrow;
    ImVec2 paneOrigin{0.0f, 0.0f};
    bool taken = false;
};

NoteReadings& noteReadings()
{
    static NoteReadings instance;

    return instance;
}

void paneNoteGui(ImGuiTestContext*)
{
    ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f), ImGuiCond_Always);
    ImGui::Begin("PaneNote", nullptr, ImGuiWindowFlags_NoSavedSettings);
    NoteReadings& taken = noteReadings();
    // Every pane starts at the same corner, so a difference between readings can
    // only come from the size.
    taken.paneOrigin = ImGui::GetCursorScreenPos();
    taken.small = drawnNote(ImVec2(300.0f, 200.0f));
    taken.wider = drawnNote(ImVec2(400.0f, 200.0f));
    taken.taller = drawnNote(ImVec2(300.0f, 300.0f));
    taken.narrow = drawnNote(ImVec2(40.0f, 200.0f));
    taken.taken = true;
    ImGui::End();
}

/// SYMPTOM IF BROKEN: the note explaining an empty scope sits in a corner of the
/// pane, or drifts as the pane is resized, instead of standing where the missing
/// trace would be.
///
/// Asserted as a difference rather than as an absolute position: glyph quads
/// carry their own side bearings, so where the ink starts is a font's business,
/// but the same words in a pane 100 wider must stand exactly 50 further across
/// and not a pixel further down.
void thePaneNoteStandsWhereTheTraceWouldBe(ImGuiTestContext* ctx)
{
    ctx->Yield();  // one frame, so the GuiFunc has drawn and measured
    const NoteReadings& taken = noteReadings();
    IM_CHECK(taken.taken);
    IM_CHECK(taken.small.vertices > 0);

    IM_CHECK_EQ(taken.wider.min.x - taken.small.min.x, 50.0f);
    IM_CHECK_EQ(taken.wider.min.y - taken.small.min.y, 0.0f);
    IM_CHECK_EQ(taken.taller.min.y - taken.small.min.y, 50.0f);
    IM_CHECK_EQ(taken.taller.min.x - taken.small.min.x, 0.0f);

    // And it stands inside the pane it names, clear of both edges.
    IM_CHECK(taken.small.min.x >= taken.paneOrigin.x + RowSeparation);
    IM_CHECK(taken.small.max.x <= taken.paneOrigin.x + 300.0f - RowSeparation);

    // SYMPTOM IF BROKEN: a pane dragged narrow shows a truncated instruction.
    // Nothing at all reaches the toolkit there - not a clipped quad.
    IM_CHECK_EQ(taken.narrow.vertices, 0);
}

/// SYMPTOM IF BROKEN: the region toolbox drops to a second row and climbs back
/// a few seconds later, while the window has not changed size.
///
/// The toolbox holds a constant width, so which row it sits on may depend only
/// on the window and on the standing furniture beside it. A line of text that
/// comes and goes to its left is enough to reflow it - the second block states
/// exactly that, and it is why a message that shows for a moment is drawn on
/// the status bar rather than up here.
void theToolboxSeatsByTheWindowAlone(ImGuiTestContext*)
{
    constexpr float Scopes = 30.0f;
    constexpr float Toolbox = 100.0f;
    const float exact = Scopes + Toolbox + RowSeparation;

    IM_CHECK(!regionToolboxWraps(Scopes, Toolbox, exact));
    IM_CHECK(!regionToolboxWraps(Scopes, Toolbox, exact + 1.0f));
    IM_CHECK(regionToolboxWraps(Scopes, Toolbox, exact - 1.0f));

    // The toolbox keeps its own gap: a row wide enough for the two clusters
    // touching still wraps.
    IM_CHECK(regionToolboxWraps(Scopes, Toolbox, Scopes + Toolbox));

    // Anything transient standing to its left moves it. On a row that seats the
    // toolbox, a notice's width wraps it - and the row springs back when the
    // notice goes.
    constexpr float Notice = 180.0f;
    IM_CHECK(regionToolboxWraps(Scopes + Notice, Toolbox, exact));
}

void registerLayoutTests(ImGuiTestEngine* engine)
{
    ImGuiTest* wholePixels = IM_REGISTER_TEST(engine, "layout", "glyph_seats_on_whole_pixels");
    wholePixels->TestFunc = glyphSeatsOnWholePixels;

    ImGuiTest* centreLine = IM_REGISTER_TEST(engine, "layout", "row_shares_one_centre_line");
    centreLine->TestFunc = rowElementsShareOneCentreLine;

    ImGuiTest* columns = IM_REGISTER_TEST(engine, "layout", "readout_columns_bind_to_labels");
    columns->TestFunc = readoutColumnsBindValuesToTheirOwnLabel;

    ImGuiTest* strip = IM_REGISTER_TEST(engine, "layout", "status_row_centred_in_strip");
    strip->TestFunc = statusRowSitsCentredInItsStrip;

    ImGuiTest* origin = IM_REGISTER_TEST(engine, "layout", "row_origin_ignores_first_element");
    origin->GuiFunc = rowOriginGui;
    origin->TestFunc = rowOriginIgnoresWhichElementComesFirst;

    ImGuiTest* scaled = IM_REGISTER_TEST(engine, "layout", "seating_holds_at_every_scale");
    scaled->TestFunc = seatingHoldsAtEveryScale;

    ImGuiTest* note = IM_REGISTER_TEST(engine, "layout", "pane_note_gives_way_before_it_is_cut");
    note->TestFunc = thePaneNoteGivesWayBeforeItIsCut;

    ImGuiTest* seated = IM_REGISTER_TEST(engine, "layout", "pane_note_stands_where_the_trace_would_be");
    seated->GuiFunc = paneNoteGui;
    seated->TestFunc = thePaneNoteStandsWhereTheTraceWouldBe;

    ImGuiTest* toolbox = IM_REGISTER_TEST(engine, "layout", "toolbox_seats_by_the_window_alone");
    toolbox->TestFunc = theToolboxSeatsByTheWindowAlone;
}

}  // namespace
}  // namespace sidescopes

int main()
{
    using namespace sidescopes;

    return uitest::runSuite("layout", registerLayoutTests, /*expectedTests=*/9);
}
