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
#include "app/imgui_ui.h"
#include "app/interface_style.h"
#include "app/scope_view.h"
#include "app/ui_scaling.h"
#include "app/window_place.h"
#include "core/environment.h"
#include "core/page_allocator.h"
#include "core/scopes/histogram.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"
#include "platform/desktop.h"

namespace {

using namespace sidescopes;

// The monitor list belongs to the toolkit rather than to any window, so its
// callback carries no window to recover state through the way every other one
// here does. One file-scope hook stands in, set beside the window callbacks
// and outlived by nothing: the state it points at is the shell's own, and the
// shell owns the toolkit for as long as it runs.
AppCallbackState* g_monitorCallbackState = nullptr;

struct MonitorWorkArea
{
    int x;
    int y;
    int width;
    int height;
};

// The display carrying most of the window; the primary when the window overlaps
// none, and null when the toolkit lists no display at all.
GLFWmonitor* monitorUnderWindow(GLFWwindow* window)
{
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    if (!monitors || monitorCount == 0) {
        return nullptr;
    }

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    glfwGetWindowPos(window, &x, &y);
    glfwGetWindowSize(window, &width, &height);
    GLFWmonitor* best = monitors[0];
    long long bestOverlap = 0;
    for (int index = 0; index < monitorCount; ++index) {
        int monitorX = 0;
        int monitorY = 0;
        int monitorWidth = 0;
        int monitorHeight = 0;
        glfwGetMonitorWorkarea(monitors[index], &monitorX, &monitorY, &monitorWidth, &monitorHeight);
        const long long overlapWidth =
            std::min(static_cast<long long>(x) + width, static_cast<long long>(monitorX) + monitorWidth) -
            std::max(x, monitorX);
        const long long overlapHeight =
            std::min(static_cast<long long>(y) + height, static_cast<long long>(monitorY) + monitorHeight) -
            std::max(y, monitorY);
        const long long overlap = std::max<long long>(overlapWidth, 0) * std::max<long long>(overlapHeight, 0);
        if (overlap > bestOverlap) {
            bestOverlap = overlap;
            best = monitors[index];
        }
    }

    return best;
}

MonitorWorkArea workAreaOf(GLFWmonitor* monitor)
{
    MonitorWorkArea work{};
    glfwGetMonitorWorkarea(monitor, &work.x, &work.y, &work.width, &work.height);

    return work;
}

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
void restoreWindowPlacement(GLFWwindow* window, const Preferences& startup)
{
    GLFWmonitor* display = nullptr;
    if (startup.windowPosition) {
        glfwSetWindowPos(window, startup.windowPosition->x, startup.windowPosition->y);
        display = monitorUnderWindow(window);
        if (display != nullptr) {
            const MonitorWorkArea work = workAreaOf(display);
            glfwSetWindowSize(window, std::clamp(startup.windowWidth, 1, std::max(1, work.width)),
                              std::clamp(startup.windowHeight, 1, std::max(1, work.height)));
        }
    } else {
        display = glfwGetPrimaryMonitor();
        if (display != nullptr) {
            const MonitorWorkArea work = workAreaOf(display);
            const WindowPlacement initial = starterWindowPlacement(
                WindowPlacement{work.x, work.y, work.width, work.height}, startup.windowWidth, startup.windowHeight);
            glfwSetWindowSize(window, initial.width, initial.height);
            glfwSetWindowPos(window, initial.x, initial.y);
        }
    }

    if (display == nullptr) {
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

    const MonitorWorkArea work = workAreaOf(display);
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

void stampInputEvent(GLFWwindow* window)
{
    static_cast<AppCallbackState*>(glfwGetWindowUserPointer(window))->lastInputEvent = glfwGetTime();
}

// Every window event the user causes stamps one clock, which is what tells the
// frame loop the interface still has frames to do. Installed before the ImGui
// backend, which chains these rather than replacing them.
void installInputClock(GLFWwindow* window)
{
    glfwSetCursorPosCallback(window, [](GLFWwindow* target, double, double) { stampInputEvent(target); });
    glfwSetCursorEnterCallback(window, [](GLFWwindow* target, int) { stampInputEvent(target); });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* target, int, int, int) { stampInputEvent(target); });
    glfwSetScrollCallback(window, [](GLFWwindow* target, double, double) { stampInputEvent(target); });
    glfwSetKeyCallback(window, [](GLFWwindow* target, int, int, int, int) { stampInputEvent(target); });
    glfwSetCharCallback(window, [](GLFWwindow* target, unsigned int) { stampInputEvent(target); });
    glfwSetWindowFocusCallback(window, [](GLFWwindow* target, int) { stampInputEvent(target); });
}

// Adds one font from a file mapped read-only instead of read into the heap.
//
// AddFontFromFileTTF copies the whole file into private dirty memory that the
// atlas keeps for the process's life, because 1.92 rasterizes glyphs on demand
// and needs the source bytes to stay put. Measured on macOS: 4.27 MB for the
// interface face and 224 KB for the monospace one, live for the whole session.
// Mapped instead, the same bytes are clean and file-backed - shared with every
// other process using the system font, and not charged to phys_footprint.
//
// The mapping is deliberately never released. It has to outlive the atlas,
// which lives as long as the interface does, and the pages cost nothing to
// keep: unmapping them at shutdown would only add a way to get it wrong.
ImFont* addMappedFont(ImGuiIO& io, const std::string& path, float sizePixels, const ImFontConfig& base)
{
    const sidescopes::MappedFile mapping = sidescopes::mapFileReadOnly(path.c_str());
    if (!mapping.valid()) {
        return nullptr;
    }
    ImFontConfig config = base;
    // ImGui must neither free these bytes nor write to them. It does not: the
    // stb_truetype loader takes the blob as const and only reads it. The
    // mapping is PROT_READ, so were that ever to change the result would be an
    // immediate fault rather than silent corruption.
    config.FontDataOwnedByAtlas = false;
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(mapping.data),
                                                  static_cast<int>(mapping.size), sizePixels, &config);
    if (font == nullptr) {
        sidescopes::unmapFile(mapping);
    }

    return font;
}

}  // namespace

namespace sidescopes {

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
    for (const std::string& path : interfaceFontFiles()) {
        if (addMappedFont(io, path, InterfaceFontSize, config) != nullptr) {
            break;
        }
    }
    ImFont* monospace = nullptr;
    const float monoSize = InterfaceFontSize * monospaceFontScale();
    for (const std::string& path : monospaceFontFiles()) {
        monospace = addMappedFont(io, path, monoSize, config);
        if (monospace != nullptr) {
            break;
        }
    }

    return monospace;
}

ImFont* startImGui(GLFWwindow* window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is ours to persist
    installInterfaceErrorReporting();
    ImGui::StyleColorsDark();
    applyTheme();

    return loadInterfaceFont(window);
}

void stopRendering(GLFWwindow* window, GraphicsBackend* graphics)
{
    if (graphics != nullptr) {
        graphics->shutdown();
    }
    ImGui::DestroyContext();
    // The monitor callback outlives the window it was installed beside, so the
    // state it reaches is dropped before the shell's own goes out of scope.
    glfwSetMonitorCallback(nullptr);
    g_monitorCallbackState = nullptr;
    glfwDestroyWindow(window);
    glfwTerminate();
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

float startupUiScaleFactor(const Preferences& startup, GLFWwindow* window)
{
    // A file that names a factor keeps it, whatever the display says. The
    // recommendation is only what a first run starts from, and the first save
    // writes it down as any other chosen value.
    if (startup.uiScaleFactor > 0.0f) {
        return startup.uiScaleFactor;
    }
    GLFWmonitor* display = monitorUnderWindow(window);
    if (display == nullptr) {
        return 1.0f;
    }
    int physicalWidthMm = 0;
    glfwGetMonitorPhysicalSize(display, &physicalWidthMm, nullptr);
    const GLFWvidmode* mode = glfwGetVideoMode(display);
    if (mode == nullptr) {
        return 1.0f;
    }

    return recommendedUiScaleFactor(mode->width, physicalWidthMm, computeUiScale(window));
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
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    if (primary == nullptr) {
        glfwTerminate();

        return {};
    }
    const MonitorWorkArea work = workAreaOf(primary);
    const int width = std::clamp(startup.windowWidth, 1, std::max(1, work.width));
    const int height = std::clamp(startup.windowHeight, 1, std::max(1, work.height));
    GLFWwindow* window = glfwCreateWindow(width, height, "SideScopes", nullptr, nullptr);
    if (!window) {
        glfwTerminate();

        return {};
    }
    // The escape hatch for GLFW's non-capturing C callbacks: they recover the
    // state through this pointer.
    glfwSetWindowUserPointer(window, &callbackState);
    restoreWindowPlacement(window, startup);
    glfwShowWindow(window);
    // AppKit recenters a newly shown GLFW window. Reapply placement before
    // the first frame so saved coordinates and the first-run position survive
    // the transition from hidden to visible.
    restoreWindowPlacement(window, startup);
    // A development build wears its version in the title bar; a release keeps
    // the plain name. Deterministic product captures ask for the release title
    // so documentation does not carry a local hash or become stale at the next
    // version.
    if (version.development && environmentValue("SIDESCOPES_PLAIN_TITLE").empty()) {
        glfwSetWindowTitle(window, ("SideScopes " + version.display).c_str());
    }
    glfwSetWindowIconifyCallback(window, [](GLFWwindow* iconifyTarget, int) {
        static_cast<AppCallbackState*>(glfwGetWindowUserPointer(iconifyTarget))->iconifyChanged.store(true);
    });
    // A display change can make capture available again; skip the recovery
    // backoff instead of waiting for its normal retry.
    g_monitorCallbackState = &callbackState;
    glfwSetMonitorCallback([](GLFWmonitor*, int) {
        if (g_monitorCallbackState != nullptr) {
            g_monitorCallbackState->displaysChanged.store(true);
        }
    });
    installInputClock(window);

    return {window, std::move(graphics)};
}

void seedImageSizes(AnalysisSettings& analysis)
{
    analysis.imageSizes[VectorscopeScopeId] = {DefaultVectorscopeSize, DefaultVectorscopeSize};
    for (const std::string_view id : WaveformFamily) {
        analysis.imageSizes[std::string{id}] = {DefaultWaveformColumns, WaveformLevels};
    }
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

ScopeTextureSet createScopeTextures(const ScopeRegistry& registry)
{
    // A slot per module scope, keyed by id and empty until a pass composes an
    // image: the upload path creates the texture at whatever size the worker
    // actually produced, and a scope with no region has no texture at all.
    // The color picker has no descriptor and draws no texture.
    ScopeTextureSet set;
    for (const HostScope& scope : registry.scopes()) {
        if (scope.descriptor != nullptr) {
            set.textures[scope.id] = nullptr;
        }
    }
    set.panePoints.assign(registry.scopes().size(), ImVec2());
    for (std::size_t i = 0; i < registry.scopes().size(); ++i) {
        set.paneIds.push_back("##pane" + std::to_string(i));
        set.dividerIds.push_back("##divider" + std::to_string(i));
    }

    return set;
}

}  // namespace sidescopes
