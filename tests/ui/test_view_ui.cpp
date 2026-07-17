// Headless tests of the app's interaction model - ScopeView and PinBoard -
// driven through a representative Dear ImGui UI on the null backend. The Test
// Engine clicks the harness controls; each test asserts the model state after
// the interaction, pinning the click -> state CONTRACTS the shipped toolbar and
// pin ring rely on.
//
// HONEST CONSTRAINT: the real main() frame loop is not yet drivable (the
// App/runFrame extraction is Phase B, in progress). These tests drive the real
// app-state classes through a REPRESENTATIVE UI built in the test's GuiFunc -
// letter buttons stand in for the toolbar and keyboard shortcuts, fixed-label
// buttons stand in for the pin ring's controls - not the shipped layout. When
// runFrame lands, these upgrade to driving the real loop; the model assertions
// carry over unchanged.
//
// Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO, used under
// its Free License; fetched at build time, never vendored.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "app/pin_board.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/frame.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "modules/module_registry.h"
#include "ui_test_harness.h"

namespace sidescopes {
namespace {

// The real interaction model under test. A function-local static keeps it
// reachable from the engine's captureless GuiFunc/TestFunc.
struct View
{
    ScopeRegistry scopeRegistry{builtinModules()};
    ScopeView scopeView{scopeRegistry};
    PinBoard pinBoard;
};

View& view()
{
    static View instance;

    return instance;
}

// The representative UI: scope letter chips wired to the real ScopeView, and
// fixed-label buttons wired to the real PinBoard's controls.
void viewGui(ImGuiTestContext*)
{
    View& h = view();

    ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f), ImGuiCond_Always);
    ImGui::Begin("View", nullptr, ImGuiWindowFlags_NoSavedSettings);

    // Letter chips stand in for the toolbar and the keyboard shortcuts: a
    // click toggles the scope in the stack, exactly as V/W/R/H/C do.
    for (const HostScope& scope : h.scopeRegistry.scopes()) {
        if (scope.letter == 0) {
            continue;
        }
        const char label[2] = {scope.letter, '\0'};
        if (ImGui::Button(label)) {
            h.scopeView.toggle(scope.id);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    // Fixed-label pin controls, so the driver targets them by name regardless
    // of how many colors are pinned.
    if (ImGui::Button("PinRed")) {
        h.pinBoard.pin(FloatColor{255.0f, 0.0f, 0.0f});
    }
    ImGui::SameLine();
    if (ImGui::Button("PinGreen")) {
        h.pinBoard.pin(FloatColor{0.0f, 255.0f, 0.0f});
    }
    ImGui::SameLine();
    if (ImGui::Button("PinBlue")) {
        h.pinBoard.pin(FloatColor{0.0f, 0.0f, 255.0f});
    }

    if (ImGui::Button("RemoveFirst")) {
        h.pinBoard.removeAt(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("SelectFirst")) {
        h.pinBoard.selectComparator(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        h.pinBoard.clear();
    }

    ImGui::Text("stack=%s pins=%d comparator=%d", h.scopeView.stackTokens().c_str(),
                static_cast<int>(h.pinBoard.size()), h.pinBoard.comparator());

    ImGui::End();
}

void scopeToggles(ImGuiTestContext* ctx)
{
    View& h = view();
    h.scopeView.restoreStack("V");  // known starting stack
    ctx->SetRef("View");

    const auto enables = [&](std::string_view id) {
        const std::vector<std::string> ids = h.scopeView.enabledScopeIds();

        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };

    // Clicking W stacks the waveform and the enabled ids follow.
    ctx->ItemClick("W");
    IM_CHECK(h.scopeView.shows(WaveformScopeId));
    IM_CHECK(h.scopeView.stack().size() == 2);
    IM_CHECK(enables(WaveformScopeId));

    // Clicking H stacks the histogram in activation order after W.
    ctx->ItemClick("H");
    IM_CHECK(h.scopeView.shows(HistogramScopeId));
    IM_CHECK(h.scopeView.stackTokens() == "VWH");

    // Clicking W again toggles the waveform back off.
    ctx->ItemClick("W");
    IM_CHECK(!h.scopeView.shows(WaveformScopeId));
    IM_CHECK(h.scopeView.stackTokens() == "VH");
    IM_CHECK(!enables(WaveformScopeId));
}

void pinAddRemove(ImGuiTestContext* ctx)
{
    View& h = view();
    h.pinBoard.clear();
    ctx->SetRef("View");

    ctx->ItemClick("PinRed");
    ctx->ItemClick("PinGreen");
    ctx->ItemClick("PinBlue");
    IM_CHECK(h.pinBoard.size() == 3);
    // The freshest pin becomes the comparison reference.
    IM_CHECK(h.pinBoard.comparator() == 2);
    IM_CHECK(h.pinBoard.hasComparator());
    IM_CHECK(h.pinBoard.comparatorColor().b == 255.0f);

    // Removing the oldest shifts the comparator index down but keeps blue as
    // the selected reference.
    ctx->ItemClick("RemoveFirst");
    IM_CHECK(h.pinBoard.size() == 2);
    IM_CHECK(h.pinBoard.comparator() == 1);
    IM_CHECK(h.pinBoard.comparatorColor().b == 255.0f);
}

void comparatorSelection(ImGuiTestContext* ctx)
{
    View& h = view();
    h.pinBoard.clear();
    ctx->SetRef("View");

    ctx->ItemClick("PinRed");    // index 0
    ctx->ItemClick("PinGreen");  // index 1, the fresh comparator

    // Selecting the oldest makes it the comparison reference.
    ctx->ItemClick("SelectFirst");
    IM_CHECK(h.pinBoard.comparator() == 0);
    IM_CHECK(h.pinBoard.comparatorColor().r == 255.0f);

    // Clearing empties the ring and drops the comparator.
    ctx->ItemClick("Clear");
    IM_CHECK(h.pinBoard.empty());
    IM_CHECK(!h.pinBoard.hasComparator());
}

void registerViewTests(ImGuiTestEngine* engine)
{
    ImGuiTest* toggles = IM_REGISTER_TEST(engine, "view", "scope_toggles");
    toggles->GuiFunc = viewGui;
    toggles->TestFunc = scopeToggles;

    ImGuiTest* pins = IM_REGISTER_TEST(engine, "view", "pin_add_remove");
    pins->GuiFunc = viewGui;
    pins->TestFunc = pinAddRemove;

    ImGuiTest* comparator = IM_REGISTER_TEST(engine, "view", "comparator_selection");
    comparator->GuiFunc = viewGui;
    comparator->TestFunc = comparatorSelection;
}

}  // namespace
}  // namespace sidescopes

int main()
{
    using namespace sidescopes;

    return uitest::runSuite("view", registerViewTests, /*expectedTests=*/3);
}
