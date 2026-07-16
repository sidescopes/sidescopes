#include "ui_test_harness.h"

#include <cstdio>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_impl_null.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_exporters.h"  // ImGuiTestEngine_PrintResultSummary

namespace sidescopes::uitest {

int runSuite(const char* suiteName, RegisterFn registerTests, int expectedTests)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplNull_Init();

    // The null backend supplies no display size or frame time. The feature
    // suites read the content region to drive adaptive scope resolution and
    // step time through ctx->Yield(), so give the headless surface a fixed,
    // deterministic size and tick.
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;

    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& testIo = ImGuiTestEngine_GetIO(engine);
    testIo.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    testIo.ConfigNoThrottle = true;
    testIo.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    testIo.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Info;
    testIo.ConfigLogToTTY = true;

    registerTests(engine);
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, nullptr, ImGuiTestRunFlags_RunFromCommandLine);
    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());

    // Pump headless frames until the queued tests finish. A TestFunc may spin
    // many ctx->Yield() frames while it polls the worker thread, so the cap is
    // generous; ConfigNoThrottle keeps the frames cheap. An empty queue means
    // every queued test finished; the cap only guards a hung test.
    constexpr int MaxFrames = 100000;
    int frame = 0;
    for (; frame < MaxFrames; ++frame) {
        ImGui_ImplNull_NewFrame();
        if (ImGuiTestEngine_IsTestQueueEmpty(engine)) {
            break;
        }

        io.DisplaySize = ImVec2(1280.0f, 800.0f);
        io.DeltaTime = 1.0f / 60.0f;
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

    const bool ranExpected = summary.CountTested == expectedTests;
    const bool allPassed = summary.CountSuccess == summary.CountTested;
    const bool timedOut = frame >= MaxFrames;
    if (!ranExpected || !allPassed || timedOut) {
        std::fprintf(stderr, "UI suite '%s' FAILED: tested=%d expected=%d success=%d frames=%d timed_out=%d\n",
                     suiteName, summary.CountTested, expectedTests, summary.CountSuccess, frame, timedOut ? 1 : 0);
        return 1;
    }

    std::printf("UI suite '%s' passed: tested=%d success=%d frames=%d\n", suiteName, summary.CountTested,
                summary.CountSuccess, frame);
    return 0;
}

}  // namespace sidescopes::uitest
