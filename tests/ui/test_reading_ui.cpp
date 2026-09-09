// Exercise the shipped pane renderer and status bar on the existing null
// backend. Texture IDs remain observable in ImGui's actual draw commands;
// no device, capture permission or parallel presentation logic is needed.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/capture_controller.h"
#include "app/region_picker.h"
#include "app/scope_pane_renderer.h"
#include "core/frame_mailbox.h"
#include "desktop_stubs.h"
#include "fake_capture.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "platform/desktop.h"
#include "ui_test_harness.h"

namespace sidescopes {

std::vector<uint8_t> rasterizeIcon(Icon, int sizePixels)
{
    return std::vector<uint8_t>(static_cast<std::size_t>(sizePixels) * static_cast<std::size_t>(sizePixels) * 4U);
}

// The granted fake capture source never shows the permission help page.
void openScreenRecordingSettings()
{
}

namespace {

constexpr ImTextureID TraceTextureId = 53001;

class RecordedTexture final : public ScopeTexture
{
public:
    RecordedTexture(int width, int height)
        : m_width(width),
          m_height(height)
    {
    }

    void upload(const ScopeImage&) override
    {
    }

    [[nodiscard]] ImTextureID textureId() const override
    {
        return TraceTextureId;
    }

    [[nodiscard]] int width() const override
    {
        return m_width;
    }

    [[nodiscard]] int height() const override
    {
        return m_height;
    }

private:
    int m_width;
    int m_height;
};

class RecordedGraphics final : public GraphicsBackend
{
public:
    int creations = 0;

    void setWindowHints() override
    {
    }

    [[nodiscard]] bool init(GLFWwindow*) override
    {
        return true;
    }

    void shutdown() override
    {
    }

    std::unique_ptr<ScopeTexture> createScopeTexture(int width, int height) override
    {
        ++creations;
        return std::make_unique<RecordedTexture>(width, height);
    }

    [[nodiscard]] bool beginFrame(int, int) override
    {
        return true;
    }

    void endFrame() override
    {
    }

    [[nodiscard]] void* nativeWindowHandle() const override
    {
        return nullptr;
    }
};

struct DrawnReading
{
    unsigned int imageIndices = 0;
    int vertices = 0;
    int graticuleVertices = 0;
};

struct ReadingHarness
{
    RecordedGraphics graphics;
    ScopeRegistry registry{builtinModules()};
    ScopeView view{registry};
    ShortcutResolver shortcuts{registry};
    AnalysisSettings analysis;
    AnalysisWorker::Output output;
    test::FakeCaptureSource source;
    FrameMailbox mailbox;
    AnalysisWorker worker{mailbox};
    CaptureController capture{source, mailbox};
    RegionPicker picker{capture, worker, source};
    PinBoard pins;
    std::unique_ptr<ScopePaneRenderer> renderer;
    bool visible = true;
    std::optional<FloatColor> marker;
    const std::optional<FloatColor> readout = FloatColor{25.5f, 51.0f, 76.5f};
    std::string statusText;
    DrawnReading drawn;
    double seconds = 100.0;

    ReadingHarness()
    {
        test::desktopStubs().reset();
        test::desktopStubs().clock = [this] { return seconds; };
        source.targets = {test::makeTarget(1, "Reading fixture")};
        (void)capture.requestPermission();
        (void)capture.start();
        std::map<std::string, ScopeInstance> projections;
        ScopeTextureSet textures;
        for (const auto& scope : registry.scopes()) {
            if (scope.descriptor) {
                projections.emplace(scope.id, builtinModules().createInstance(scope.id));
                textures.textures.emplace(scope.id, nullptr);
                output.images[scope.id] = {std::vector<uint8_t>(16, 255), 2, 2, 1};
            }
            textures.panePoints.emplace_back();
            textures.paneIds.push_back("##reading-pane" + std::to_string(textures.paneIds.size()));
            textures.dividerIds.push_back("##reading-divider" + std::to_string(textures.dividerIds.size()));
        }
        output.outlines[HistogramScopeId] = std::vector<float>(std::size_t{3} * Histogram::Bins, 0.5f);
        renderer = std::make_unique<ScopePaneRenderer>(
            ScopePaneContext{graphics, view, registry, analysis, output, capture, picker, pins, shortcuts},
            std::move(projections), std::move(textures));
        renderer->configureProjections();
    }

    void show(std::string_view id)
    {
        view.stack().restore("[" + std::string(id) + "]");
        renderer->uploadVisibleScopes(true);
        visible = true;
        marker.reset();
    }
};

ReadingHarness& harness()
{
    static ReadingHarness instance;
    return instance;
}

DrawnReading readPaneCommands(const ImGuiWindow* parent)
{
    DrawnReading result;
    // The scope area owns the child windows. Status icons share the fake
    // backend but draw only in the parent row, outside this measurement.
    for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
        if (!window->Active || window == parent || window->RootWindow != parent) {
            continue;
        }
        const auto& draw = *window->DrawList;
        result.vertices += draw.VtxBuffer.Size;
        for (const auto& command : draw.CmdBuffer) {
            if (command.GetTexID() == TraceTextureId) {
                result.imageIndices += command.ElemCount;
            }
        }
        for (const auto& vertex : draw.VtxBuffer) {
            if (vertex.col == GraticuleMajor || vertex.col == GraticuleLabel || vertex.col == GraticuleMinor) {
                ++result.graticuleVertices;
            }
        }
    }
    return result;
}

void readingGui(ImGuiTestContext*)
{
    auto& h = harness();
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(600, 600), ImGuiCond_Always);
    ImGui::Begin("Reading", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    const auto* parent = ImGui::GetCurrentWindow();
    const PaneRenderInput input{1.0f, h.visible, true, h.marker, h.marker, h.readout, nullptr};
    (void)h.renderer->drawScopePanes(input);
    h.drawn = readPaneCommands(parent);
    ImGui::LogToBuffer();
    h.renderer->drawStatusBar(input);
    h.statusText = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
}

void hiddenReadingsKeepOnlyTheGraticule(ImGuiTestContext* ctx)
{
    auto& h = harness();
    IM_CHECK(h.capture.permissionGranted() && !h.capture.dead());
    for (const auto* id : {VectorscopeScopeId, WaveformScopeId, LumaWaveformScopeId, ParadeScopeId, HistogramScopeId}) {
        h.show(id);
        ctx->Yield(2);
        IM_CHECK_GT(h.drawn.imageIndices, 0u);
        const int withoutMarker = h.drawn.vertices;
        h.marker = FloatColor{220, 50, 80};
        ctx->Yield(2);
        IM_CHECK_GT(h.drawn.vertices, withoutMarker);
        h.visible = false;
        ctx->Yield(2);
        IM_CHECK_EQ(h.drawn.imageIndices, 0u);
        IM_CHECK_GT(h.drawn.graticuleVertices, 0);
        const int hiddenWithOldMarker = h.drawn.vertices;
        h.marker.reset();
        ctx->Yield(2);
        IM_CHECK_EQ(h.drawn.vertices, hiddenWithOldMarker);
        if (std::string_view(id) == HistogramScopeId) {
            auto outline = std::move(h.output.outlines[HistogramScopeId]);
            h.output.outlines[HistogramScopeId].clear();
            ctx->Yield(2);
            IM_CHECK_EQ(h.drawn.vertices, hiddenWithOldMarker);
            h.output.outlines[HistogramScopeId] = std::move(outline);
        }
        h.visible = true;
        ctx->Yield(2);
        IM_CHECK_GT(h.drawn.imageIndices, 0u);
        IM_CHECK_EQ(h.drawn.vertices, withoutMarker);
    }
}

void hiddenUploadsCannotRecreateReleasedTraces(ImGuiTestContext* ctx)
{
    auto& h = harness();
    IM_CHECK(h.capture.permissionGranted() && !h.capture.dead());
    h.show(VectorscopeScopeId);
    ctx->Yield(2);
    IM_CHECK_GT(h.drawn.imageIndices, 0u);
    h.renderer->releaseTraces();
    const int before = h.graphics.creations;
    h.renderer->uploadVisibleScopes(false);
    IM_CHECK_EQ(h.graphics.creations, before);
    ctx->Yield(2);
    IM_CHECK_EQ(h.drawn.imageIndices, 0u);
    h.renderer->uploadVisibleScopes(true);
    IM_CHECK_EQ(h.graphics.creations, before + 1);
    ctx->Yield(2);
    IM_CHECK_GT(h.drawn.imageIndices, 0u);
}

void registerReadingTests(ImGuiTestEngine* engine)
{
    auto* visibility = IM_REGISTER_TEST(engine, "reading", "hidden_readings_keep_only_the_graticule");
    visibility->GuiFunc = readingGui;
    visibility->TestFunc = hiddenReadingsKeepOnlyTheGraticule;
    auto* upload = IM_REGISTER_TEST(engine, "reading", "hidden_uploads_cannot_recreate_released_traces");
    upload->GuiFunc = readingGui;
    upload->TestFunc = hiddenUploadsCannotRecreateReleasedTraces;
}

}  // namespace
}  // namespace sidescopes

int main()
{
    return sidescopes::uitest::runSuite("reading", sidescopes::registerReadingTests, 2);
}
