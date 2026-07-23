#include "app/app_startup.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "app/scope_view.h"
#include "app/ui_scaling.h"
#include "core/scopes/histogram.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"
#include "platform/desktop.h"

namespace {

using namespace sidescopes;

// Applies the saved window placement and keeps the window on screen.
//
// Saved sizes are in the platform's own window units (physical pixels on
// Windows, points on macOS), so they are restored with an explicit set
// after creation: passing them through glfwCreateWindow instead would
// run them through GLFW_SCALE_TO_MONITOR's creation-time scaling on
// Windows, growing the window by the monitor scale on every launch.
// Position first, size second - crossing into a differently scaled
// monitor triggers the hint's automatic resize, and the explicit size
// must land after it.
//
// The rectangle is then clamped into the work area of the monitor it
// mostly lies on. A window that starts beyond the desktop edge shows
// its never-composited strip as white while a drag holds the frame
// loop; a window that never starts off screen has no such strip.
struct MonitorWorkArea
{
    int x;
    int y;
    int width;
    int height;
};

// The work area of the monitor carrying most of the window; the primary when
// the window overlaps none.
MonitorWorkArea monitorMostlyContaining(GLFWmonitor** monitors, int monitorCount, int x, int y, int width, int height)
{
    int workX = 0;
    int workY = 0;
    int workWidth = 0;
    int workHeight = 0;
    glfwGetMonitorWorkarea(monitors[0], &workX, &workY, &workWidth, &workHeight);
    long long bestOverlap = 0;
    for (int index = 0; index < monitorCount; ++index) {
        int monitorX = 0;
        int monitorY = 0;
        int monitorWidth = 0;
        int monitorHeight = 0;
        glfwGetMonitorWorkarea(monitors[index], &monitorX, &monitorY, &monitorWidth, &monitorHeight);
        const long long overlapWidth = std::min<long long>(x + width, monitorX + monitorWidth) - std::max(x, monitorX);
        const long long overlapHeight =
            std::min<long long>(y + height, monitorY + monitorHeight) - std::max(y, monitorY);
        const long long overlap = std::max<long long>(overlapWidth, 0) * std::max<long long>(overlapHeight, 0);
        if (overlap > bestOverlap) {
            bestOverlap = overlap;
            workX = monitorX;
            workY = monitorY;
            workWidth = monitorWidth;
            workHeight = monitorHeight;
        }
    }

    return {workX, workY, workWidth, workHeight};
}

void restoreWindowPlacement(GLFWwindow* window, const Preferences& startup)
{
    if (startup.windowX >= 0) {
        glfwSetWindowPos(window, startup.windowX, startup.windowY);
        glfwSetWindowSize(window, startup.windowWidth, startup.windowHeight);
    }

    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    if (!monitors || monitorCount == 0) {
        return;
    }

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    glfwGetWindowPos(window, &x, &y);
    glfwGetWindowSize(window, &width, &height);
    int frameLeft = 0;
    int frameTop = 0;
    int frameRight = 0;
    int frameBottom = 0;
    glfwGetWindowFrameSize(window, &frameLeft, &frameTop, &frameRight, &frameBottom);

    const MonitorWorkArea work = monitorMostlyContaining(monitors, monitorCount, x, y, width, height);
    const int availableWidth = std::max(1, work.width - frameLeft - frameRight);
    const int availableHeight = std::max(1, work.height - frameTop - frameBottom);
    const int clampedWidth = std::min(width, availableWidth);
    const int clampedHeight = std::min(height, availableHeight);
    const int minX = work.x + frameLeft;
    const int maxX = work.x + work.width - frameRight - clampedWidth;
    const int minY = work.y + frameTop;
    const int maxY = work.y + work.height - frameBottom - clampedHeight;
    const int clampedX = std::max(minX, std::min(x, maxX));
    const int clampedY = std::max(minY, std::min(y, maxY));
    if (clampedWidth != width || clampedHeight != height) {
        glfwSetWindowSize(window, clampedWidth, clampedHeight);
    }
    if (clampedX != x || clampedY != y) {
        glfwSetWindowPos(window, clampedX, clampedY);
    }
}

std::unique_ptr<ScopeTexture> createBlankTexture(GraphicsBackend& graphics, int width, int height)
{
    auto texture = graphics.createScopeTexture(width, height);
    ScopeImage blank;
    blank.width = width;
    blank.height = height;
    blank.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    for (std::size_t i = 3; i < blank.rgba.size(); i += 4) {
        blank.rgba[i] = 255;
    }
    texture->upload(blank);

    return texture;
}

}  // namespace

namespace sidescopes {

void applyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.WindowPadding = ImVec2(10, 8);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    ImVec4* colors = style.Colors;
    const ImVec4 background(0.0f, 0.0f, 0.0f, 1.0f);
    const ImVec4 panel(0.13f, 0.13f, 0.14f, 1.0f);
    const ImVec4 hovered(0.20f, 0.20f, 0.22f, 1.0f);
    const ImVec4 active(0.26f, 0.27f, 0.30f, 1.0f);
    const ImVec4 accent(0.28f, 0.42f, 0.65f, 1.0f);
    colors[ImGuiCol_WindowBg] = background;
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.11f, 0.98f);
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgActive] = background;
    colors[ImGuiCol_FrameBg] = panel;
    colors[ImGuiCol_FrameBgHovered] = hovered;
    colors[ImGuiCol_FrameBgActive] = active;
    colors[ImGuiCol_Button] = panel;
    colors[ImGuiCol_ButtonHovered] = hovered;
    colors[ImGuiCol_ButtonActive] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.36f, 0.52f, 0.78f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.70f, 0.95f, 1.0f);
    colors[ImGuiCol_Text] = ImVec4(0.86f, 0.86f, 0.88f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.53f, 1.0f);
}

ImFont* loadInterfaceFont(GLFWwindow* window)
{
    int windowWidth = 0;
    int framebufferWidth = 0;
    glfwGetWindowSize(window, &windowWidth, nullptr);
    glfwGetFramebufferSize(window, &framebufferWidth, nullptr);
    ImFontConfig config;
    config.RasterizerDensity = interfaceFontDensity(windowWidth, framebufferWidth);
    // ImGui's default range stops at U+00FF, which would drop the delta the
    // color picker labels its differences with. Latin-1 plus that one glyph.
    static constexpr ImWchar InterfaceGlyphRanges[] = {0x0020, 0x00FF, 0x0394, 0x0394, 0};
    config.GlyphRanges = InterfaceGlyphRanges;
    ImGuiIO& io = ImGui::GetIO();
    bool loaded = false;
    for (const std::string& path : interfaceFontFiles()) {
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), InterfaceFontSize, &config)) {
            loaded = true;
            break;
        }
    }
    ImFont* monospace = nullptr;
    const float monoSize = InterfaceFontSize * monospaceFontScale();
    for (const std::string& path : monospaceFontFiles()) {
        monospace = io.Fonts->AddFontFromFileTTF(path.c_str(), monoSize, &config);
        if (monospace != nullptr) {
            break;
        }
    }
    (void)loaded;

    return monospace;
}

float computeUiScale(GLFWwindow* window)
{
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    int windowWidth = 0;
    int framebufferWidth = 0;
    glfwGetWindowSize(window, &windowWidth, nullptr);
    glfwGetFramebufferSize(window, &framebufferWidth, nullptr);

    return uiScaleForWindow(scaleX, windowWidth, framebufferWidth);
}

MainWindow createMainWindow(const Preferences& startup, const VersionInfo& version, AppCallbackState& callbackState)
{
    auto graphics = createGraphicsBackend();
    graphics->setWindowHints();
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    // On Windows the window follows its monitor's scale, so it keeps its
    // physical size when dragged between differently scaled monitors; macOS
    // ignores the hint (scaling lives in the framebuffer there).
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    // Hidden until the saved placement is applied: geometry settles before the
    // first paint, and no intermediate rectangle flashes.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(startup.windowWidth, startup.windowHeight, "SideScopes", nullptr, nullptr);
    if (!window) {
        glfwTerminate();

        return {};
    }
    // The escape hatch for GLFW's non-capturing C callbacks: they recover the
    // state through this pointer.
    glfwSetWindowUserPointer(window, &callbackState);
    restoreWindowPlacement(window, startup);
    glfwShowWindow(window);
    // A development build wears its version in the title bar; a release keeps
    // the plain name.
    if (version.development) {
        glfwSetWindowTitle(window, ("SideScopes " + version.display).c_str());
    }
    glfwSetWindowIconifyCallback(window, [](GLFWwindow* iconifyTarget, int) {
        static_cast<AppCallbackState*>(glfwGetWindowUserPointer(iconifyTarget))->iconifyChanged.store(true);
    });

    return {window, std::move(graphics)};
}

void seedAnalysis(AnalysisSettings& analysis, const Preferences& startup)
{
    // The worker is driven entirely by scope id: each scope's saved parameters
    // fan out by key straight from the preference map into the module's
    // declarative vocabulary. Smoothing rides the same map but belongs to the
    // host, so it is filtered out. The parade shares the waveform's gain and
    // stride, so both are re-seeded from the waveform.
    for (const auto& [id, params] : startup.scopeParams) {
        for (const auto& [key, value] : params) {
            if (key != "smoothing_ms") {
                analysis.scopeParams[id][key] = value;
            }
        }
    }
    analysis.scopeParams[ParadeScopeId]["gain"] = analysis.scopeParams[WaveformScopeId]["gain"];
    analysis.scopeParams[ParadeScopeId]["stride"] = analysis.scopeParams[WaveformScopeId]["stride"];
    analysis.imageSizes[VectorscopeScopeId] = {DefaultVectorscopeSize, DefaultVectorscopeSize};
    analysis.imageSizes[WaveformScopeId] = {DefaultWaveformColumns, WaveformLevels};
    analysis.imageSizes[ParadeScopeId] = {DefaultWaveformColumns, WaveformLevels};
    analysis.imageSizes[HistogramScopeId] = {Histogram::ImageWidth, Histogram::Height};
}

std::map<std::string, ScopeInstance> createProjectionInstances(const ScopeRegistry& registry)
{
    // Projection instances place the overlays and markers on the main thread:
    // one module instance per scope, drawing declarative graticule primitives
    // and cursor markers. They never accumulate. The color picker has no module
    // instance, so it is skipped here and drawn as host state.
    std::map<std::string, ScopeInstance> instances;
    for (const HostScope& scope : registry.scopes()) {
        if (scope.descriptor != nullptr) {
            instances.emplace(scope.id, builtinModules().createInstance(scope.id));
        }
    }

    return instances;
}

ScopeTextureSet createScopeTextures(GraphicsBackend& graphics, const ScopeRegistry& registry)
{
    // One texture per module scope, keyed by id and sized from its descriptor;
    // the upload path resizes to whatever the worker actually produces. The
    // color picker has no descriptor and draws no texture.
    ScopeTextureSet set;
    for (const HostScope& scope : registry.scopes()) {
        if (!scope.descriptor) {
            continue;
        }
        const int width = scope.descriptor->image_width > 0 ? scope.descriptor->image_width : DefaultVectorscopeSize;
        const int height = scope.descriptor->image_height > 0 ? scope.descriptor->image_height : DefaultVectorscopeSize;
        set.textures[scope.id] = createBlankTexture(graphics, width, height);
    }
    set.panePoints.assign(registry.scopes().size(), ImVec2());
    for (std::size_t i = 0; i < registry.scopes().size(); ++i) {
        set.paneIds.push_back("##pane" + std::to_string(i));
        set.dividerIds.push_back("##divider" + std::to_string(i));
    }

    return set;
}

}  // namespace sidescopes
