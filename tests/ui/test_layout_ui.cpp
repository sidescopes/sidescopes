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
#include "app/menu_rows.h"
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

/// SYMPTOM IF BROKEN: the preset button sits a shade off its neighbour on the
/// toolbar - a taller box, a glyph on a different line, or its own row shifting
/// as the drift star arrives and leaves.
///
/// The preset picker carries a value beside its glyph; the scope selector
/// beside it does not. They must still read as one pair: same height, same
/// glyph seat, wider only by the value - and by the width of the WIDEST value,
/// never by the one being shown, because everything to their right moves with
/// them.
void labelledIconSeatsLikeItsPlainSibling(ImGuiTestContext* ctx)
{
    const auto check = [] {
        const float side = ImGui::GetTextLineHeight();
        const float widest = ImGui::CalcTextSize("9*").x;
        const float width = labelledIconButtonWidth(widest);

        // Never narrower than the plain button beside it, and never so narrow
        // that the label would print over the glyph.
        IM_CHECK_GT(width, iconButtonWidth());
        IM_CHECK_GT(iconButtonWidth(), iconButtonInset() + side);

        // The margin past the label matches the one before the glyph, so the
        // ink is inset the same at both ends of the box.
        IM_CHECK_EQ(width - iconButtonWidth() - widest, iconButtonInset());
        IM_CHECK_GT(iconButtonInset(), 0.0f);

        // The box is sized to the WIDEST label, not the one being shown: the
        // two differ, which is the whole cost of getting that wrong - the row
        // shifting every time the drift star arrives or leaves.
        IM_CHECK_GT(width, labelledIconButtonWidth(ImGui::CalcTextSize("9").x));
    };

    check();

    // The rule is a ratio of the line height, so it holds at every interface
    // scale - including the extremes, where a fixed margin would vanish.
    const ImGuiStyle saved = ImGui::GetStyle();
    for (const float scale : {0.5f, 2.0f}) {
        applyInterfaceScale(scale);
        ctx->Yield();
        check();
    }
    ImGui::GetStyle() = saved;
    ctx->Yield();
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

// What the chosen-row marks actually put in the draw list, read back from the
// vertices they appended, and where a row-leading icon button leaves the
// cursor with its glyph painted and with it hidden.
struct ChosenProbe
{
    ImU32 bandColor = 0;
    int plainKeyVertices = -1;
    int chosenKeyVertices = -1;
    ImVec2 paintedIconSize{-1.0f, -1.0f};
    ImVec2 hiddenIconSize{-1.0f, -1.0f};
    float paintedNameX = -1.0f;
    float hiddenNameX = -1.0f;
    int paintedIconVertices = -1;
    int hiddenIconVertices = -1;
};

ChosenProbe& chosenProbe()
{
    static ChosenProbe instance;

    return instance;
}

/// Draws the chosen-row band and both readings of a row's key, recording what
/// each emitted.
void chosenBandGui(ImGuiTestContext*)
{
    ImGui::SetNextWindowSize(ImVec2(300.0f, 120.0f), ImGuiCond_Always);
    ImGui::Begin("Bands", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ChosenProbe& probe = chosenProbe();

    int before = draw->VtxBuffer.Size;
    drawMenuRowChosen(ImGui::GetCursorScreenPos().y);
    probe.bandColor = draw->VtxBuffer.Size > before ? draw->VtxBuffer[before].col : 0;

    // Both keys are drawn over a laid-out item, which is what they align to.
    ImGui::Selectable("row", false, ImGuiSelectableFlags_None, ImVec2(200.0f, ImGui::GetFrameHeight()));
    before = draw->VtxBuffer.Size;
    drawMenuRowAccelerator("4", ImGui::GetFontSize());
    probe.plainKeyVertices = draw->VtxBuffer.Size - before;

    before = draw->VtxBuffer.Size;
    drawMenuRowChosenKey("4", ImGui::GetFontSize());
    probe.chosenKeyVertices = draw->VtxBuffer.Size - before;

    // The same row-leading button drawn both ways, each followed by the name
    // that stands after it, so the test can compare where that name landed.
    const auto probeRow = [draw](const char* id, bool painted, ImVec2& size, float& nameX, int& vertices) {
        const int mark = draw->VtxBuffer.Size;
        const bool pressed = menuRowIconButton(id, ImTextureID{}, "Rename", painted);
        IM_UNUSED(pressed);
        size = ImGui::GetItemRectSize();
        vertices = draw->VtxBuffer.Size - mark;
        ImGui::SameLine(menuRowNameX());
        ImGui::TextUnformatted("Preset 1");
        nameX = ImGui::GetItemRectMin().x;
    };
    probeRow("##painted", true, probe.paintedIconSize, probe.paintedNameX, probe.paintedIconVertices);
    probeRow("##hidden", false, probe.hiddenIconSize, probe.hiddenNameX, probe.hiddenIconVertices);
    ImGui::End();
}

/// SYMPTOM IF BROKEN: in the preset list, the row the pointer is over looks
/// exactly like the row that is loaded - so the list appears to have two
/// selections, or the real one becomes invisible under the pointer.
///
/// Which row is loaded is carried by a band rather than by a marker glyph, and
/// the hover is a band too. The two must be different shades, and the chosen
/// band must be see-through enough that a chosen row under the pointer is
/// different again from either.
void theChosenBandIsNotTheHoverBand(ImGuiTestContext* ctx)
{
    ctx->SetRef("Bands");
    ctx->Yield();
    const ChosenProbe& probe = chosenProbe();

    // It drew something at all.
    IM_CHECK_NE(probe.bandColor, 0u);
    // ...and not the hover band's colour, which is what drawMenuRowHover puts
    // down on the very same rectangle.
    IM_CHECK_NE(probe.bandColor, ImGui::GetColorU32(ImGuiCol_ButtonHovered));
    // ...and it is see-through, so hovering the chosen row adds to it rather
    // than replacing it.
    const ImU32 alpha = probe.bandColor >> IM_COL32_A_SHIFT & 0xFFu;
    IM_CHECK_GT(alpha, 0u);
    IM_CHECK_LT(alpha, 255u);
}

/// SYMPTOM IF BROKEN: which preset is loaded is told only by a colour, so a
/// reader who cannot separate that colour from the plain one has no cue at all.
///
/// The chosen row's key is a filled badge, not the same glyph in another
/// colour. The badge is a SHAPE, so it must put more in the draw list than the
/// bare text does - a recolour would emit exactly as much.
void theChosenKeyIsAShapeAndNotAColour(ImGuiTestContext* ctx)
{
    ctx->SetRef("Bands");
    ctx->Yield();
    const ChosenProbe& probe = chosenProbe();

    IM_CHECK_GT(probe.plainKeyVertices, 0);
    IM_CHECK_GT(probe.chosenKeyVertices, probe.plainKeyVertices);
}

/// SYMPTOM IF BROKEN: every name in the preset list steps sideways as the
/// pointer runs down it, and the whole menu ripples under the cursor.
///
/// The rename pen shows only on the row under the pointer. A hover-revealed
/// control must still OCCUPY its space when it is not drawn - reserving the box
/// only while it shows is the classic form of this bug, and one that looks
/// perfectly right in a still picture and is unusable in motion. Only the paint
/// may differ between the two states; the layout may not.
void aHiddenRowIconStillHoldsItsSpace(ImGuiTestContext* ctx)
{
    ctx->SetRef("Bands");
    ctx->Yield();
    const ChosenProbe& probe = chosenProbe();

    // The box is real in both states...
    IM_CHECK_GT(probe.paintedIconSize.x, 0.0f);
    IM_CHECK_EQ(probe.paintedIconSize.x, probe.hiddenIconSize.x);
    IM_CHECK_EQ(probe.paintedIconSize.y, probe.hiddenIconSize.y);

    // ...and so the name after it lands at the very same x either way, which
    // is the thing the eye would catch.
    IM_CHECK_GT(probe.paintedNameX, 0.0f);
    IM_CHECK_EQ(probe.paintedNameX, probe.hiddenNameX);

    // What DOES differ is the paint, or the reveal would not be a reveal.
    IM_CHECK_GT(probe.paintedIconVertices, probe.hiddenIconVertices);
    IM_CHECK_EQ(probe.hiddenIconVertices, 0);

    // The name column clears the button that leads the row, so the two never
    // overlap however either is measured.
    IM_CHECK_GT(menuRowNameX(), menuRowIconWidth());
    IM_CHECK_GT(menuRowKeyRightPad(), 0.0f);
}

void registerLayoutTests(ImGuiTestEngine* engine)
{
    ImGuiTest* wholePixels = IM_REGISTER_TEST(engine, "layout", "glyph_seats_on_whole_pixels");
    wholePixels->TestFunc = glyphSeatsOnWholePixels;

    ImGuiTest* centreLine = IM_REGISTER_TEST(engine, "layout", "row_shares_one_centre_line");
    centreLine->TestFunc = rowElementsShareOneCentreLine;

    ImGuiTest* labelled = IM_REGISTER_TEST(engine, "layout", "labelled_icon_seats_like_its_sibling");
    labelled->TestFunc = labelledIconSeatsLikeItsPlainSibling;

    ImGuiTest* columns = IM_REGISTER_TEST(engine, "layout", "readout_columns_bind_to_labels");
    columns->TestFunc = readoutColumnsBindValuesToTheirOwnLabel;

    ImGuiTest* strip = IM_REGISTER_TEST(engine, "layout", "status_row_centred_in_strip");
    strip->TestFunc = statusRowSitsCentredInItsStrip;

    ImGuiTest* origin = IM_REGISTER_TEST(engine, "layout", "row_origin_ignores_first_element");
    origin->GuiFunc = rowOriginGui;
    origin->TestFunc = rowOriginIgnoresWhichElementComesFirst;

    ImGuiTest* scaled = IM_REGISTER_TEST(engine, "layout", "seating_holds_at_every_scale");
    scaled->TestFunc = seatingHoldsAtEveryScale;

    ImGuiTest* toolbox = IM_REGISTER_TEST(engine, "layout", "toolbox_seats_by_the_window_alone");
    toolbox->TestFunc = theToolboxSeatsByTheWindowAlone;

    ImGuiTest* chosen = IM_REGISTER_TEST(engine, "layout", "chosen_band_is_not_the_hover_band");
    chosen->GuiFunc = chosenBandGui;
    chosen->TestFunc = theChosenBandIsNotTheHoverBand;

    ImGuiTest* badge = IM_REGISTER_TEST(engine, "layout", "chosen_key_is_a_shape");
    badge->GuiFunc = chosenBandGui;
    badge->TestFunc = theChosenKeyIsAShapeAndNotAColour;

    ImGuiTest* reveal = IM_REGISTER_TEST(engine, "layout", "hidden_row_icon_holds_its_space");
    reveal->GuiFunc = chosenBandGui;
    reveal->TestFunc = aHiddenRowIconStillHoldsItsSpace;
}

}  // namespace
}  // namespace sidescopes

int main()
{
    using namespace sidescopes;

    return uitest::runSuite("layout", registerLayoutTests, /*expectedTests=*/11);
}
