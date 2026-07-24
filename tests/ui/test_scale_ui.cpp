// Headless asserts that the interface scale never compounds - the defect that
// left the chrome permanently half-size after one visit to the 50% step.
//
// These are invariants, not goldens: applyInterfaceScale rebuilds from the
// unscaled base every time, so re-applying any scale must reproduce the same
// style, and Default must return to the base metrics - on any platform, with
// any font.
//
// The fields checked are the ones ScaleAllSizes multiplies but the theme does
// NOT name, so a missing reset would compound exactly here; the nine fields the
// theme sets are restored by re-theming and cannot show the defect.
//
// Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO, used under
// its Free License; fetched at build time, never vendored.

#include "app/interface_style.h"
#include "imgui.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "ui_test_harness.h"

namespace sidescopes {
namespace {

/// SYMPTOM IF BROKEN: opening the UI Scaling menu twice leaves the chrome the
/// wrong size, and 50% shrinks scrollbars and sliders for the rest of the run.
///
/// applyInterfaceScale rebuilds from the unscaled base, so applying a scale is
/// idempotent whatever preceded it: 150% reached through Default reads exactly
/// as 150% reached fresh. Without the reset, ScaleAllSizes would multiply the
/// already-scaled sizes and its truncation would never unwind.
void reapplyingAScaleNeverCompounds(ImGuiTestContext*)
{
    const ImGuiStyle saved = ImGui::GetStyle();

    applyInterfaceScale(1.5f);
    const ImGuiStyle fresh = ImGui::GetStyle();
    applyInterfaceScale(1.0f);
    applyInterfaceScale(1.5f);
    const ImGuiStyle reached = ImGui::GetStyle();

    ImGui::GetStyle() = saved;

    IM_CHECK_EQ(reached.ScrollbarSize, fresh.ScrollbarSize);
    IM_CHECK_EQ(reached.GrabMinSize, fresh.GrabMinSize);
    IM_CHECK_EQ(reached.IndentSpacing, fresh.IndentSpacing);
    IM_CHECK_EQ(reached.ItemInnerSpacing.x, fresh.ItemInnerSpacing.x);
    IM_CHECK_EQ(reached.CellPadding.x, fresh.CellPadding.x);
}

/// SYMPTOM IF BROKEN: after visiting 50%, returning to Default leaves the
/// chrome permanently smaller than it started.
///
/// The base metrics ScaleAllSizes leaves untouched at 1.0 must survive a visit
/// to a fractional step - the ImTrunc inside ScaleAllSizes would otherwise
/// round the shrunk sizes down and never restore them.
void defaultReturnsToTheBaseMetrics(ImGuiTestContext*)
{
    const ImGuiStyle saved = ImGui::GetStyle();

    applyInterfaceScale(1.0f);
    const ImGuiStyle base = ImGui::GetStyle();
    applyInterfaceScale(0.5f);
    applyInterfaceScale(1.0f);
    const ImGuiStyle returned = ImGui::GetStyle();

    ImGui::GetStyle() = saved;

    IM_CHECK_EQ(returned.ScrollbarSize, base.ScrollbarSize);
    IM_CHECK_EQ(returned.GrabMinSize, base.GrabMinSize);
    IM_CHECK_EQ(returned.IndentSpacing, base.IndentSpacing);
    IM_CHECK_EQ(returned.ItemInnerSpacing.x, base.ItemInnerSpacing.x);
    IM_CHECK_EQ(returned.CellPadding.x, base.CellPadding.x);
}

void registerScaleTests(ImGuiTestEngine* engine)
{
    ImGuiTest* noCompound = IM_REGISTER_TEST(engine, "scale", "reapplying_never_compounds");
    noCompound->TestFunc = reapplyingAScaleNeverCompounds;

    ImGuiTest* toBase = IM_REGISTER_TEST(engine, "scale", "default_returns_to_base");
    toBase->TestFunc = defaultReturnsToTheBaseMetrics;
}

}  // namespace
}  // namespace sidescopes

int main()
{
    using namespace sidescopes;

    return uitest::runSuite("scale", registerScaleTests, /*expectedTests=*/2);
}
