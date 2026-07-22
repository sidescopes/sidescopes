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

#include <cmath>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
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
}

}  // namespace
}  // namespace sidescopes

int main()
{
    using namespace sidescopes;

    return uitest::runSuite("layout", registerLayoutTests, /*expectedTests=*/5);
}
