// Headless asserts on the drawn context menu - the fallback the application
// shows where no native menu is available (a headless run, or a Wayland
// session with no XWayland, where GTK cannot start). It drives the real
// src/app/imgui_context_menu.cpp pair: openImGuiContextMenu with a built item
// list, then drawImGuiContextMenu once per frame, and it clicks a top-level
// action, a nested submenu action, and a dismissal - the three paths a menu
// choice can take through drawItems.
//
// WHY A UI SUITE, not a unit test: the whole point is that a right-click
// produces the RIGHT action id after ImGui's popup and submenu machinery has
// had its say. Asserting drawItems in isolation would test a switch statement;
// asserting through the engine tests the menu the user actually clicks. What it
// does NOT cover is that the app's right-click handler calls this pair - that
// needs a graphics backend and the whole App, neither of which exists
// headlessly - so, as with the reorder suite, this drives the shipped menu
// against a stand-in item list rather than the shipped right-click.
//
// Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO, used under
// its Free License; fetched at build time, never vendored.

#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "app/imgui_context_menu.h"
#include "imgui.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "platform/native_menu.h"
#include "ui_test_harness.h"

namespace sidescopes {
namespace {

// Action ids the harness menu carries, distinct so a wrong dispatch is
// visible rather than coincidentally right.
constexpr int ActionTop = 11;
constexpr int ActionNested = 22;
constexpr int ActionOther = 33;

/// The menu the tests drive and the choice the last frame reported. A
/// function-local static keeps it reachable from the engine's captureless
/// GuiFunc and TestFunc.
struct MenuState
{
    int lastChosen = -1;
    bool everOpen = false;
    bool openRequested = false;
};

MenuState& state()
{
    static MenuState menu;
    return menu;
}

/// The stand-in item list: one plain action, a submenu holding another, and a
/// trailing action, so a top-level pick, a nested pick and the walk PAST a
/// submenu are all exercised.
std::vector<NativeMenuItem> harnessItems()
{
    std::vector<NativeMenuItem> items;
    items.push_back({NativeMenuItem::Kind::Action, "Top Action", ActionTop, false, "T"});
    items.push_back({NativeMenuItem::Kind::SubmenuBegin, "More", -1, false, ""});
    items.push_back({NativeMenuItem::Kind::Action, "Nested Action", ActionNested, false, ""});
    items.push_back({NativeMenuItem::Kind::SubmenuEnd, "", -1, false, ""});
    items.push_back({NativeMenuItem::Kind::Separator, "", -1, false, ""});
    items.push_back({NativeMenuItem::Kind::Action, "Other Action", ActionOther, false, ""});
    return items;
}

void guiFunc(ImGuiTestContext*)
{
    ImGui::SetNextWindowSize(ImVec2(300, 300), ImGuiCond_Always);
    ImGui::Begin("Menu Host", nullptr, ImGuiWindowFlags_NoSavedSettings);
    if (state().openRequested) {
        openImGuiContextMenu(harnessItems());
        state().openRequested = false;
    }
    const ImGuiContextMenuFrame frame = drawImGuiContextMenu();
    if (frame.open) {
        state().everOpen = true;
    }
    if (frame.chosen >= 0) {
        state().lastChosen = frame.chosen;
    }
    ImGui::End();
}

/// Opens the menu fresh for one case and clears the recorded choice.
void openMenu(ImGuiTestContext* ctx)
{
    state().lastChosen = -1;
    state().everOpen = false;
    state().openRequested = true;
    ctx->Yield(2);
}

void registerTests(ImGuiTestEngine* engine)
{
    ImGuiTest* topTest = IM_REGISTER_TEST(engine, "context_menu", "top_level_action");
    topTest->GuiFunc = guiFunc;
    topTest->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("Menu Host");
        openMenu(ctx);
        IM_CHECK(state().everOpen);
        ctx->SetRef("//$FOCUSED");
        ctx->ItemClick("Top Action");
        IM_CHECK_EQ(state().lastChosen, ActionTop);
    };

    ImGuiTest* nestedTest = IM_REGISTER_TEST(engine, "context_menu", "nested_submenu_action");
    nestedTest->GuiFunc = guiFunc;
    nestedTest->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("Menu Host");
        openMenu(ctx);
        // The submenu is its own popup window, so it is opened first and the
        // nested action reached by a search across every window.
        ctx->SetRef("//$FOCUSED");
        ctx->ItemOpen("More");
        ctx->Yield(2);
        ctx->ItemClick("**/Nested Action");
        IM_CHECK_EQ(state().lastChosen, ActionNested);
    };

    ImGuiTest* dismissTest = IM_REGISTER_TEST(engine, "context_menu", "dismiss_leaves_no_choice");
    dismissTest->GuiFunc = guiFunc;
    dismissTest->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->SetRef("Menu Host");
        openMenu(ctx);
        IM_CHECK(state().everOpen);
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK_EQ(state().lastChosen, -1);
    };
}

}  // namespace
}  // namespace sidescopes

int main()
{
    return sidescopes::uitest::runSuite("context_menu", sidescopes::registerTests, 3);
}
