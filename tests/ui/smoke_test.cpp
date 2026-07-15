// Headless smoke test for the Dear ImGui Test Engine harness.
//
// Proves the harness runs against the same Dear ImGui the application uses:
// it builds a throwaway window with a button, has the engine click the button,
// and checks that a flag flipped. It runs on Dear ImGui's null (headless)
// backend and returns a non-zero exit code on any failure. It intentionally
// does not touch SideScopes' real UI yet.
//
// Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO, used under its
// Free License (this is a natural-person, publicly released open-source project;
// see the fetched imgui_test_engine/LICENSE.txt). It is fetched at build time
// and never vendored into this repository.

#include <cstdio>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_impl_null.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_exporters.h"  // ImGuiTestEngine_PrintResultSummary

namespace {
/// Flipped by the button's GuiFunc, asserted by the TestFunc. File scope so a
/// captureless lambda (plain function pointer, the engine's default) can reach it.
bool g_buttonClicked = false;

void registerSmokeTest(ImGuiTestEngine* engine)
{
    ImGuiTest* test = IM_REGISTER_TEST(engine, "smoke", "button_click");

    test->GuiFunc = [](ImGuiTestContext*) {
        ImGui::Begin("Smoke Window", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::Button("Smoke Button")) {
            g_buttonClicked = true;
        }

        ImGui::End();
    };

    test->TestFunc = [](ImGuiTestContext* ctx) {
        g_buttonClicked = false;
        ctx->SetRef("Smoke Window");
        ctx->ItemClick("Smoke Button");
        IM_CHECK(g_buttonClicked);
    };
}
}  // namespace

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplNull_Init();

    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& testIo = ImGuiTestEngine_GetIO(engine);
    testIo.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    testIo.ConfigNoThrottle = true;
    testIo.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    testIo.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Info;
    testIo.ConfigLogToTTY = true;

    registerSmokeTest(engine);
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, nullptr, ImGuiTestRunFlags_RunFromCommandLine);
    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());

    // Pump headless frames until the queued test finishes. The queue stays
    // non-empty for the duration of the run, so an empty queue means done; the
    // frame cap keeps a hung test from spinning forever.
    constexpr int MaxFrames = 1000;
    int frame = 0;
    for (; frame < MaxFrames; ++frame) {
        ImGui_ImplNull_NewFrame();
        if (ImGuiTestEngine_IsTestQueueEmpty(engine)) {
            break;
        }

        ImGui::NewFrame();
        ImGui::Render();
        ImGui_ImplNullRender_RenderDrawData(ImGui::GetDrawData());
        ImGuiTestEngine_PostSwap(engine);
    }

    ImGuiTestEngine_Stop(engine);

    ImGuiTestEngineResultSummary summary;
    ImGuiTestEngine_GetResultSummary(engine, &summary);
    ImGuiTestEngine_PrintResultSummary(engine);

    // Destroy the Dear ImGui context before the test engine context so the
    // engine can flush its ini data on shutdown.
    ImGui_ImplNull_Shutdown();
    ImGui::DestroyContext();
    ImGuiTestEngine_DestroyContext(engine);

    const bool ranExactlyOne = summary.CountTested == 1;
    const bool allPassed = summary.CountSuccess == summary.CountTested;
    const bool timedOut = frame >= MaxFrames;
    if (!ranExactlyOne || !allPassed || timedOut) {
        std::fprintf(stderr, "UI smoke test FAILED: tested=%d success=%d frames=%d timed_out=%d\n", summary.CountTested,
                     summary.CountSuccess, frame, timedOut ? 1 : 0);
        return 1;
    }

    std::printf("UI smoke test passed: tested=%d success=%d frames=%d\n", summary.CountTested, summary.CountSuccess,
                frame);
    return 0;
}
