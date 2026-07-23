// Headless end-to-end test of the real scope pipeline driven through a
// representative Dear ImGui UI on the null backend.
//
// It wires the actual application pieces together - a CaptureController over a
// fake capture source (the control plane), a FrameMailbox and an AnalysisWorker
// running the real scope engines through the module boundary (the data plane),
// and a ScopeView (the interaction model) - and drives them with the Test
// Engine. There is no GPU on the null backend, so the assertions read the
// worker's Output DATA (the CPU-side scope images), not uploaded textures.
//
// HONEST CONSTRAINT: the real main() frame loop is not yet drivable (the
// App/runFrame extraction is Phase B, in progress). This test drives the real
// pipeline and the real app-state classes through a REPRESENTATIVE UI built in
// the test's GuiFunc, not the shipped window layout. When runFrame lands, this
// upgrades to driving the real loop; the pipeline and model assertions carry
// over unchanged.
//
// Dear ImGui Test Engine (c) 2018-2026 Omar Cornut / DISCO HELLO, used under
// its Free License; fetched at build time, never vendored.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "app/capture_controller.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/analysis_worker.h"
#include "core/frame_mailbox.h"
#include "fake_capture.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "modules/module_registry.h"
#include "scope_image.h"
#include "test_frame.h"
#include "ui_test_harness.h"

namespace sidescopes {
namespace {

using namespace test;

// The real application pieces under test, wired the way the app wires them.
// A function-local static keeps them reachable from the engine's captureless
// GuiFunc/TestFunc (plain function pointers, like the smoke test's flag).
struct Pipeline
{
    FrameMailbox mailbox;
    FakeCaptureSource source;
    CaptureController controller{source, mailbox};
    AnalysisWorker worker{mailbox};
    ScopeRegistry scopeRegistry{builtinModules()};
    ScopeView scopeView{scopeRegistry};

    // The settings last synced into the worker; the GuiFunc keeps them in
    // step with the view and the pane size, as the app's frame loop does.
    AnalysisSettings settings;
    int settingsPushes = 0;

    // A display-only view of the worker output for the on-screen readout,
    // tracked on its own version cursor so it never fights the test's fetch.
    uint64_t displayVersion = 0;
    AnalysisWorker::Output displayOutput;
};

Pipeline& pipeline()
{
    static Pipeline instance;

    return instance;
}

// Pumps engine frames until @p predicate holds, yielding to the worker thread
// between checks. This is the frame pump, not a sleep: no wall-clock wait, and
// the worker's own seams (consumedFrameSequence, fetchOutput) decide when the
// state has caught up.
template <typename Predicate>
bool pumpUntil(ImGuiTestContext* ctx, Predicate predicate, int maxYields = 5000)
{
    for (int i = 0; i < maxYields; ++i) {
        if (predicate()) {
            return true;
        }
        ctx->Yield();
    }

    return predicate();
}

bool imagePopulated(const ScopeImage& image)
{
    return brightestPixel(image) != std::pair<int, int>{-1, -1};
}

// The scope image for @p id, or an empty image when the worker has produced
// none (a disabled scope has no output entry).
const ScopeImage& imageFor(const AnalysisWorker::Output& output, const std::string& id)
{
    static const ScopeImage empty;
    const auto at = output.images.find(id);

    return at != output.images.end() ? at->second : empty;
}

// The representative UI: a scope toolbar wired to the real ScopeView, a
// pane-size-driven adaptive settings update, and a readout of the worker's
// output - the surface the Test Engine interacts with.
void pipelineGui(ImGuiTestContext*)
{
    Pipeline& h = pipeline();

    // A fixed window size makes the content region - and thus the adaptive
    // resolution it drives - deterministic on the headless backend.
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 700.0f), ImGuiCond_Always);
    ImGui::Begin("Pipeline", nullptr, ImGuiWindowFlags_NoSavedSettings);

    // One button per stackable scope, toggling the real ScopeView exactly as
    // the app's toolbar does.
    for (const HostScope& scope : h.scopeRegistry.scopes()) {
        if (scope.letter == 0) {
            continue;
        }
        const char label[2] = {scope.letter, '\0'};
        if (ImGui::Button(label)) {
            h.scopeView.stack().toggle(scope.id);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    // The scope pane's size feeds the worker's waveform image dimensions, the
    // way the app retiers resolution on a resize. The vectorscope stays on its
    // 256-code grid so its calibrated targets hold.
    const ImVec2 pane = ImGui::GetContentRegionAvail();
    const int columns = pane.x >= 1600.0f ? 2048 : (pane.x >= 800.0f ? 1024 : 512);
    const int imageHeight = pane.y >= 512.0f ? 512 : 256;

    // Sync the view's stack and the pane-derived resolution into the worker
    // whenever either changed - the app's per-frame settings push. The
    // waveform and its parade share one image size.
    const std::vector<std::string> ids = h.scopeView.stack().enabledScopeIds();
    const std::pair<int, int> waveSize{columns, imageHeight};
    const auto currentSize = h.settings.imageSizes.find(WaveformScopeId);
    const bool sizeChanged = currentSize == h.settings.imageSizes.end() || currentSize->second != waveSize;
    if (ids != h.settings.enabledScopes || sizeChanged) {
        h.settings.enabledScopes = ids;
        h.settings.imageSizes[WaveformScopeId] = waveSize;
        h.settings.imageSizes[ParadeScopeId] = waveSize;
        h.worker.updateSettings(h.settings);
        ++h.settingsPushes;
    }

    // Show the worker's latest Output state, so an assertion has a UI to read.
    (void)h.worker.fetchOutput(h.displayVersion, h.displayOutput);
    ImGui::Text("frames=%llu waveform=%d", static_cast<unsigned long long>(h.displayOutput.framesProcessed),
                imagePopulated(imageFor(h.displayOutput, WaveformScopeId)) ? 1 : 0);

    ImGui::End();
}

void endToEnd(ImGuiTestContext* ctx)
{
    Pipeline& h = pipeline();
    ctx->SetRef("Pipeline");

    const auto enables = [&](std::string_view id) {
        const std::vector<std::string> ids = h.scopeView.stack().enabledScopeIds();

        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };

    // --- control plane: permission and start through the CaptureController ---
    IM_CHECK(h.controller.requestPermission());
    IM_CHECK(h.controller.start());
    IM_CHECK(h.controller.capturedDisplay() == 7u);
    IM_CHECK(h.controller.status() == "capturing primary");

    // One GuiFunc frame syncs the pane-derived resolution and the
    // vectorscope-only mask into the worker before any frame is analyzed.
    ctx->Yield();
    IM_CHECK(h.settingsPushes >= 1);
    IM_CHECK(h.settings.imageSizes.at(WaveformScopeId).second == 512);  // pane height flowed in
    IM_CHECK(h.scopeView.stack().enabledScopeIds() == std::vector<std::string>{VectorscopeScopeId});

    // --- data plane: a known input frame yields a known vectorscope output ---
    h.mailbox.publish(makeSolidFrameBuffer(64, 64, Color{191, 0, 0}, 1));
    // Wait on the seam, not a clock: the worker has taken the frame.
    IM_CHECK(pumpUntil(ctx, [&] { return h.worker.consumedFrameSequence() >= 1; }));

    uint64_t seen = 0;
    AnalysisWorker::Output out;
    IM_CHECK(pumpUntil(ctx, [&] { return h.worker.fetchOutput(seen, out); }));
    // 75% red lands on the default BT.709 target (bin 109, 43).
    IM_CHECK(pixelLit(imageFor(out, VectorscopeScopeId), 109, 43));
    // The waveform is off, so its image never populated.
    IM_CHECK(imageFor(out, WaveformScopeId).rgba.empty());

    // --- UI action: clicking W adds the waveform to the real ScopeView ---
    IM_CHECK(!enables(WaveformScopeId));
    ctx->ItemClick("W");
    IM_CHECK(h.scopeView.stack().shows(WaveformScopeId));
    IM_CHECK(enables(WaveformScopeId));

    // The GuiFunc pushes the new ids on its next frame; the worker recomputes
    // on the frame it already holds and the waveform image populates. This is
    // the full chain: click -> ScopeView -> settings -> worker -> engine image.
    IM_CHECK(pumpUntil(
        ctx, [&] { return h.worker.fetchOutput(seen, out) && !imageFor(out, WaveformScopeId).rgba.empty(); }));
    IM_CHECK(imagePopulated(imageFor(out, WaveformScopeId)));
    IM_CHECK(pixelLit(imageFor(out, VectorscopeScopeId), 109, 43));  // vectorscope still lit

    // --- a fresh frame through the seam changes the engine output ---
    // A different solid color must move the vectorscope trace off the red bin.
    h.mailbox.publish(makeSolidFrameBuffer(64, 64, Color{0, 191, 0}, 2));
    IM_CHECK(pumpUntil(ctx, [&] { return h.worker.consumedFrameSequence() >= 2; }));
    IM_CHECK(pumpUntil(ctx, [&] { return h.worker.fetchOutput(seen, out) && out.framesProcessed >= 2; }));
    IM_CHECK(!pixelLit(imageFor(out, VectorscopeScopeId), 109, 43));  // no longer 75% red
    IM_CHECK(imagePopulated(imageFor(out, VectorscopeScopeId)));
}

void registerPipelineTests(ImGuiTestEngine* engine)
{
    ImGuiTest* test = IM_REGISTER_TEST(engine, "pipeline", "end_to_end");
    test->GuiFunc = pipelineGui;
    test->TestFunc = endToEnd;
}

}  // namespace
}  // namespace sidescopes

int main()
try {
    using namespace sidescopes;

    Pipeline& h = pipeline();
    h.source.targets = {test::makeTarget(7, "primary")};

    // Start on the vectorscope alone, so the waveform is provably off until the
    // UI turns it on. The worker consumes frames on its own thread from here.
    h.settings.enabledScopes = h.scopeView.stack().enabledScopeIds();
    h.worker.updateSettings(h.settings);
    h.worker.start();

    return uitest::runSuite("pipeline", registerPipelineTests, /*expectedTests=*/1);
} catch (const std::exception& error) {
    std::fprintf(stderr, "pipeline setup failed: %s\n", error.what());

    return 1;
}
