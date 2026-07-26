// Headless asserts on what the graticule's strength actually puts on screen.
//
// The strength is decided in src/app/overlay_style.cpp and drawn by
// src/app/overlay_render.cpp, and only the toolkit sees both: these tests run
// the real renderer against a hooked Dear ImGui and read the colors back out of
// the draw list it filled. A styling path that forgets to lay its ink down at
// the chosen strength compiles, looks right in review, and is caught here.
//
// Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO, used under
// its Free License; fetched at build time, never vendored.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "app/overlay_render.h"
#include "app/overlay_style.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "sidescopes/module.h"
#include "ui_test_harness.h"

namespace sidescopes {
namespace {

/// One of every graticule primitive the modules emit, so the sweep covers every
/// styling path rather than the one a single scope happens to use: the
/// waveform's minor and major scale lines with their labels, the vectorscope's
/// rings, its 75% target box, and its skin-tone line.
std::vector<SsGraticulePrimitive> everyPrimitive()
{
    std::vector<SsGraticulePrimitive> primitives;

    SsGraticulePrimitive minor{};
    minor.kind = SS_PRIMITIVE_LINE;
    minor.stroke = SS_STROKE_GRID;
    minor.x0 = 0.0f;
    minor.y0 = 0.4f;
    minor.x1 = 1.0f;
    minor.y1 = 0.4f;
    primitives.push_back(minor);

    SsGraticulePrimitive major = minor;
    major.stroke = SS_STROKE_GRID_MAJOR;
    major.y0 = 0.5f;
    major.y1 = 0.5f;
    primitives.push_back(major);

    SsGraticulePrimitive skin = minor;
    skin.stroke = SS_STROKE_SKIN_TONE;
    skin.x0 = 0.5f;
    skin.y0 = 0.5f;
    skin.x1 = 0.7f;
    skin.y1 = 0.2f;
    primitives.push_back(skin);

    SsGraticulePrimitive ring{};
    ring.kind = SS_PRIMITIVE_CIRCLE;
    ring.stroke = SS_STROKE_GRID;
    ring.x0 = 0.5f;
    ring.y0 = 0.5f;
    ring.x1 = 0.25f;
    primitives.push_back(ring);

    SsGraticulePrimitive target{};
    target.kind = SS_PRIMITIVE_TARGET_BOX;
    target.stroke = SS_STROKE_ACCENT;
    target.flags = SS_PRIMITIVE_FLAG_TARGET_PRIMARY;
    target.x0 = 0.7f;
    target.y0 = 0.3f;
    std::snprintf(target.label, sizeof(target.label), "R");
    primitives.push_back(target);

    SsGraticulePrimitive text{};
    text.kind = SS_PRIMITIVE_TEXT;
    text.stroke = SS_STROKE_GRID_MAJOR;
    text.x0 = 0.0f;
    text.y0 = 0.5f;
    std::snprintf(text.label, sizeof(text.label), "50");
    primitives.push_back(text);

    return primitives;
}

/// What one graticule left in the draw list: the distinct vertex colors, and
/// how many vertices carried them.
struct DrawnInk
{
    std::vector<ImU32> colors;
    int vertices = 0;
};

/// Draws the whole primitive set at @p strength into the current window and
/// reports the ink that reached the toolkit. Anti-aliasing contributes fully
/// transparent copies of each color along the feathered edges; they are kept,
/// because scaling an alpha of zero leaves it at zero and the comparison holds.
DrawnInk drawnInk(float strength)
{
    const ImDrawList* draw = ImGui::GetWindowDrawList();
    const int before = draw->VtxBuffer.Size;
    GraticuleStyle style;
    style.strength = strength;
    // Inside the window, and well inside it: Dear ImGui culls TEXT against the
    // clip rectangle while it leaves lines and boxes to the GPU, so a scope
    // placed at arbitrary screen coordinates would silently drop every label
    // and leave these tests blind to exactly the element most likely to be
    // forgotten.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    drawGraticule(DrawnScope{origin, ImVec2(200.0f, 200.0f), 1.0f}, everyPrimitive(), style);

    DrawnInk ink;
    ink.vertices = draw->VtxBuffer.Size - before;
    for (int vertex = before; vertex < draw->VtxBuffer.Size; ++vertex) {
        const ImU32 color = draw->VtxBuffer[vertex].col;
        if (std::find(ink.colors.begin(), ink.colors.end(), color) == ink.colors.end()) {
            ink.colors.push_back(color);
        }
    }
    std::sort(ink.colors.begin(), ink.colors.end());

    return ink;
}

// The suite's readings, taken in the GuiFunc where a draw list exists and
// asserted in the TestFunc. A function-local static keeps them reachable from
// the engine's captureless callbacks.
struct Readings
{
    DrawnInk atFloor;
    DrawnInk atDefault;
    ImU32 labelInk = 0;
    bool taken = false;
};

Readings& readings()
{
    static Readings instance;

    return instance;
}

void graticuleGui(ImGuiTestContext*)
{
    ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f), ImGuiCond_Always);
    ImGui::Begin("Graticule", nullptr, ImGuiWindowFlags_NoSavedSettings);
    Readings& taken = readings();
    taken.atFloor = drawnInk(GraticuleStrengths.front());
    taken.atDefault = drawnInk(DefaultGraticuleStrength);
    taken.labelInk = graticuleInk(GraticuleLabel, DefaultGraticuleStrength);
    taken.taken = true;
    ImGui::End();
}

/// SYMPTOM IF BROKEN: a graticule element ignores the strength setting - the
/// vectorscope's target boxes stay at full brightness while its grid dims
/// around them, or a newly added primitive draws at full ink for ever.
///
/// Every color that reaches the toolkit at the floor must be the color that
/// reached it at the default, scaled. Nothing may arrive unscaled, and nothing
/// scaled may arrive that the default did not draw.
void everyElementTakesTheStrength(ImGuiTestContext* ctx)
{
    ctx->Yield();  // one frame, so the GuiFunc has drawn and measured
    const Readings& taken = readings();
    IM_CHECK(taken.taken);
    IM_CHECK(!taken.atDefault.colors.empty());
    // Non-vacuity: the label ink must be among what was read, or a clipped or
    // unrasterised glyph would leave the labels untested and the sweep would
    // pass while covering only the lines.
    IM_CHECK(std::find(taken.atDefault.colors.begin(), taken.atDefault.colors.end(), taken.labelInk) !=
             taken.atDefault.colors.end());
    IM_CHECK(std::find(taken.atFloor.colors.begin(), taken.atFloor.colors.end(),
                       graticuleInk(taken.labelInk, GraticuleStrengths.front())) != taken.atFloor.colors.end());

    std::vector<ImU32> expected;
    expected.reserve(taken.atDefault.colors.size());
    for (const ImU32 color : taken.atDefault.colors) {
        expected.push_back(graticuleInk(color, GraticuleStrengths.front()));
    }
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());

    IM_CHECK_EQ(taken.atFloor.colors.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        IM_CHECK_EQ(taken.atFloor.colors[index], expected[index]);
    }
}

/// SYMPTOM IF BROKEN: quietening the graticule takes part of it away instead of
/// dimming it - a line or a label vanishes at the lowest step and the scale can
/// no longer be read.
///
/// The floor is a strength, not a filter: the same geometry is drawn at every
/// step, so the draw list gets the same vertices either way.
void theFloorDropsNothing(ImGuiTestContext* ctx)
{
    ctx->Yield();
    const Readings& taken = readings();
    IM_CHECK(taken.taken);
    IM_CHECK(taken.atDefault.vertices > 0);
    IM_CHECK_EQ(taken.atFloor.vertices, taken.atDefault.vertices);
}

void registerGraticuleTests(ImGuiTestEngine* engine)
{
    ImGuiTest* strength = IM_REGISTER_TEST(engine, "graticule", "every_element_takes_the_strength");
    strength->GuiFunc = graticuleGui;
    strength->TestFunc = everyElementTakesTheStrength;

    ImGuiTest* floor = IM_REGISTER_TEST(engine, "graticule", "the_floor_drops_nothing");
    floor->GuiFunc = graticuleGui;
    floor->TestFunc = theFloorDropsNothing;
}

}  // namespace
}  // namespace sidescopes

int main()
{
    using namespace sidescopes;

    return uitest::runSuite("graticule", registerGraticuleTests, /*expectedTests=*/2);
}
