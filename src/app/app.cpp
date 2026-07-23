// The SideScopes application shell, shared by every platform: a compact,
// always-on-top window stacking the enabled scopes. All analysis lives in
// the core library on its own thread; this file owns the interaction
// model (gestures, native menu, region selection) and preferences, while
// rendering and window chrome live behind the graphics seam.

#include "app/app.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "app/border_label.h"
#include "app/capture_controller.h"
#include "app/color_readout.h"
#include "app/context_menu.h"
#include "app/imgui_ui.h"
#include "app/overlay_render.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/region_geometry.h"
#include "app/row_layout.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/settings_window.h"
#include "app/ui_scaling.h"
#include "app/version.h"
#include "app/window_suggestions.h"
#include "core/analysis_worker.h"
#include "core/color_lab.h"
#include "core/diagnostics.h"
#include "core/frame_mailbox.h"
#include "core/marker_smoother.h"
#include "core/preferences.h"
#include "core/region_suggestions.h"
#include "core/scopes/histogram.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"
#include "core/trace_intensity.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"
#include "platform/graphics.h"
#include "platform/native_menu.h"
#include "platform/region_selection.h"
#include "platform/screen_capture.h"
#include "sidescopes_version.h"

namespace {

using namespace sidescopes;

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

// Draws a scope image into the available space. The vectorscope keeps its
// square aspect (it is a polar plot); the waveform stretches, because its
// horizontal axis is arbitrary image columns. A zoom magnifies the view
// around the center - trace, graticule, and markers together, which is
// what keeps every overlay glued to the cloud - by cropping the
// texture's center and scaling overlay coordinates through At().
DrawnScope drawScopeImage(const ScopeTexture& texture, bool keepAspect, float zoom = 1.0f)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImVec2 size = available;
    if (keepAspect) {
        const float scale = std::max(0.05f, std::min(available.x / static_cast<float>(texture.width()),
                                                     available.y / static_cast<float>(texture.height())));
        size = ImVec2(static_cast<float>(texture.width()) * scale, static_cast<float>(texture.height()) * scale);
    }
    ImVec2 cursor = ImGui::GetCursorPos();
    cursor.x += std::max(0.0f, (available.x - size.x) * 0.5f);
    cursor.y += std::max(0.0f, (available.y - size.y) * 0.5f);
    ImGui::SetCursorPos(cursor);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float crop = 0.5f / zoom;
    ImGui::Image(texture.textureId(), size, ImVec2(0.5f - crop, 0.5f - crop), ImVec2(0.5f + crop, 0.5f + crop));
    return DrawnScope{origin, size, zoom};
}

// The champagne-gold graticule palette, the marker colors, and the
// primitive/marker translation layer live in overlay_render; only the per-scope
// pane setup below stays here. A whole-color point marker's live-cursor color is
// white, a luma level's is gold, and pinned references are amber; the graticule
// speaks the same gold at a handful of strengths for every scope.

// The cursor color a scope's markers are queried for crosses the module
// boundary as an SsColor.
SsColor toSsColor(const FloatColor& color)
{
    return SsColor{color.r, color.g, color.b};
}

/// One trace's intensity result: scroll derives the gain from the intensity,
/// while a double-click restores the default gain exactly.
struct TraceAdjustment
{
    float intensity;
    float gain;
};

/// Scroll adjusts the trace intensity, double-click restores the default gain.
/// Draws the intensity readout while this trace's flash is up. Returns the new
/// values when the user changed them.
std::optional<TraceAdjustment> traceIntensityGesture(const DrawnScope& scope, std::string_view control, float intensity,
                                                     float defaultGain, float intensityShift, TraceFlash& flash)
{
    if (!ImGui::IsItemHovered()) {
        return std::nullopt;
    }
    const ImGuiIO& io = ImGui::GetIO();
    std::optional<TraceAdjustment> adjusted;
    if (io.MouseWheel != 0.0f) {
        intensity = std::clamp(intensity + 2.0f * io.MouseWheel, 0.0f, 100.0f);
        adjusted = TraceAdjustment{intensity, traceGainFromIntensity(intensity, intensityShift)};
        flash.show(control, glfwGetTime() + 1.2);
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        intensity = intensityFromTraceGain(defaultGain, intensityShift);
        adjusted = TraceAdjustment{intensity, defaultGain};
        flash.show(control, glfwGetTime() + 1.2);
    }
    if (flash.showing(control, glfwGetTime())) {
        char text[32];
        std::snprintf(text, sizeof(text), "intensity %.0f%%", intensity);
        ImGui::GetWindowDrawList()->AddText(ImVec2(scope.origin.x + 8, scope.origin.y + 6),
                                            IM_COL32(235, 235, 240, 220), text);
    }

    return adjusted;
}

// Scope toggles are letter chips: professional tools label scopes with
// text because no icon language exists for them, and the letters double
// as the keyboard shortcuts.
bool scopeToggleButton(const char* id, const char* letter, bool enabled, const char* tooltip)
{
    const float height = ImGui::GetTextLineHeight() + 4.0f;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(height + 8.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (enabled) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonActive), 3.0f);
    } else if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    const ImU32 color = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 size = ImGui::CalcTextSize(letter);
    const ImVec2 at(std::floor(min.x + (max.x - min.x - size.x) / 2), std::floor(min.y + (max.y - min.y - size.y) / 2));
    draw->AddText(at, color, letter);
    wrappedTooltip(tooltip);
    return pressed;
}

// The pixel size the icon textures rasterize at: the toolbar square in
// framebuffer pixels, so the strokes land on the physical grid.
int iconPixelSize()
{
    return static_cast<int>(std::lround(ImGui::GetTextLineHeight() * ImGui::GetIO().DisplayFramebufferScale.x));
}

bool iconButton(const char* id, ImTextureID texture, const char* tooltip, bool dimmed = false)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(iconButtonWidth(), iconButtonHeight()));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    const float side = ImGui::GetTextLineHeight();
    const ImVec2 glyph = iconGlyphOrigin(min, max, side);
    draw->AddImage(texture, glyph, ImVec2(glyph.x + side, glyph.y + side), ImVec2(0, 0), ImVec2(1, 1),
                   ImGui::GetColorU32(ImGuiCol_Text, dimmed ? 0.4f : 1.0f));
    wrappedTooltip(tooltip);

    return pressed;
}

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

// Loads the interface font and its fixed-width companion, returning the
// monospace font so the picker can align hex codes with it; null when the
// system had none and the interface font stands in.
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

// The interface is authored in 100%-scale units. On macOS GLFW window
// coordinates are already such units - the framebuffer alone carries the
// Retina factor - so this is 1.0; on Windows the window is sized in
// physical pixels and the monitor's content scale (1.25, 1.5, ...) says
// how many of them the interface should treat as one.
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

// The histogram pane draws the filled texture, strokes each channel's curve
// over it at display resolution, then adds the graticule and cursor-value
// markers. The outline stroking is host display logic over the worker's
// extension output; the graticule and markers come from the projection
// instance's declarative primitives, like every other scope.
// The curve outline strokes at display resolution over the filled texture:
// baked into the texture it would stretch anisotropically with the pane - thick
// on flats, thin on slopes. Sampled through the same spline the fill uses, so
// line and fill edge agree.
void strokeHistogramOutline(const DrawnScope& scope, const AnalysisWorker::Output& output, HistogramStyle style,
                            std::vector<ImVec2>& points)
{
    if (output.histogramOutline.size() != static_cast<std::size_t>(3) * Histogram::Bins) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(scope.origin, ImVec2(scope.origin.x + scope.size.x, scope.origin.y + scope.size.y), true);
    const bool bands = style == HistogramStyle::PerChannel;
    const int samples = std::clamp(static_cast<int>(scope.size.x), 128, 2 * Histogram::Bins);
    for (int channel = 0; channel < 3; ++channel) {
        const float* plane = output.histogramOutline.data() + static_cast<std::ptrdiff_t>(channel) * Histogram::Bins;
        const float bandTop = scope.origin.y + (bands ? static_cast<float>(channel) * scope.size.y / 3.0f : 0.0f);
        const float bandHeight = bands ? scope.size.y / 3.0f : scope.size.y;
        points.clear();
        for (int sample = 0; sample < samples; ++sample) {
            const float binPosition =
                std::clamp((static_cast<float>(sample) + 0.5f) * Histogram::Bins / static_cast<float>(samples) - 0.5f,
                           0.0f, Histogram::Bins - 1.0f);
            const int center = static_cast<int>(binPosition);
            const float t = binPosition - static_cast<float>(center);
            const auto at = [&](int index) { return plane[std::clamp(index, 0, Histogram::Bins - 1)]; };
            const float p0 = at(center - 1);
            const float p1 = at(center);
            const float p2 = at(center + 1);
            const float p3 = at(center + 2);
            float height =
                p1 +
                0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
            if (p1 <= 0.0f && p2 <= 0.0f) {
                height = 0.0f;
            }
            height = std::clamp(height, 0.0f, 1.0f);
            // Empty stretches ride the baseline: the outline stays one continuous
            // reading of the channel. Kept just inside the band so the stroke
            // survives the clip.
            const float y = std::min(bandTop + (1.0f - height) * bandHeight, bandTop + bandHeight - 1.0f);
            points.push_back(ImVec2(
                scope.origin.x + (static_cast<float>(sample) + 0.5f) * scope.size.x / static_cast<float>(samples), y));
        }
        if (points.size() >= 2) {
            draw->AddPolyline(points.data(), static_cast<int>(points.size()),
                              channelMaskColor(1u << static_cast<uint32_t>(channel)), ImDrawFlags_None, 1.6f);
        }
    }
    draw->PopClipRect();
}

void drawHistogram(const ScopeTexture& texture, const AnalysisWorker::Output& output, const ScopeInstance& instance,
                   HistogramStyle style, bool showGraticule, const std::optional<FloatColor>& markerColor,
                   std::vector<ImVec2>& points)
{
    // No intensity gesture here: the histogram's scale adjusts
    // itself, the way every editor draws it.
    const DrawnScope scope = drawScopeImage(texture, false);
    strokeHistogramOutline(scope, output, style, points);
    if (showGraticule) {
        drawGraticule(scope, instance.graticule(), GraticuleStyle{});
    }
    if (markerColor) {
        drawMarkers(scope, instance.markers(SsColor{markerColor->r, markerColor->g, markerColor->b}));
    }
}

// When nothing can be captured, the scope area explains why and how to fix it
// instead of drawing empty instruments; a non-technical user should never face
// a blank vectorscope.
void drawCaptureHelp(const char* headline, const std::vector<std::string>& lines, bool offerSettings)
{
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float blockHeight =
        lineHeight * (2.0f + static_cast<float>(lines.size())) + (offerSettings ? lineHeight * 2.0f : 0.0f);
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (area.y - blockHeight) / 2.0f)));
    const auto centeredText = [&](const char* text) {
        const float width = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
        ImGui::TextUnformatted(text);
    };
    centeredText(headline);
    ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.4f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (const std::string& line : lines) {
        centeredText(line.c_str());
    }
    ImGui::PopStyleColor();
    if (offerSettings) {
        ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.6f));
        const char* label = "Open System Settings";
        const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
        if (ImGui::Button(label)) {
            openScreenRecordingSettings();
        }
    }
}

// A shortcut name resolves to the ImGui key it fires on; only Escape and the
// bare letters are bindable, so anything else never matches a press.
ImGuiKey keyFor(const std::string& name)
{
    if (name == "Escape") {
        return ImGuiKey_Escape;
    }
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'A'));
    }

    return ImGuiKey_None;
}

// Per-scope toolbar chrome, keyed by id: the button id, display name, and
// tooltip suffix. The shortcut is resolved by id through bindingFor.
struct ScopeChrome
{
    const char* buttonId;
    const char* name;
    const char* extra;
};

ScopeChrome scopeChromeFor(std::string_view id)
{
    if (id == VectorscopeScopeId) {
        return {"##toggle-vectorscope", "Vectorscope", ""};
    }
    if (id == WaveformScopeId) {
        return {"##toggle-waveform", "Waveform", "; styles in the right-click menu"};
    }
    if (id == ParadeScopeId) {
        return {"##toggle-waveform-parade", "RGB parade", ""};
    }
    if (id == HistogramScopeId) {
        return {"##toggle-histogram", "Histogram", ""};
    }

    return {"##toggle-color-picker", "Color picker", ""};
}

bool shortcutPressed(const std::string& binding)
{
    const ImGuiKey key = keyFor(binding);

    return key != ImGuiKey_None && ImGui::IsKeyPressed(key, false);
}

}  // namespace

namespace sidescopes {

App::App()
    : m_worker(m_mailbox),
      m_capture(createScreenCaptureSource()),
      m_captureController(*m_capture, m_mailbox),
      m_scopeRegistry(builtinModules()),
      m_view(m_scopeRegistry)
{
    m_screenSample = std::make_shared<ScreenSample>();
}

bool App::init()
{
    diagInit();
    if (!glfwInit()) {
        return false;
    }

    const Preferences startup = loadPreferences(preferencesFilePath());
    m_versionInfo = describeVersion(SIDESCOPES_VERSION, SIDESCOPES_GIT_DESCRIBE);

    if (!createMainWindow(startup)) {
        return false;
    }
    setupImGui();
    if (!m_graphics->init(m_window)) {
        ImGui::DestroyContext();
        glfwDestroyWindow(m_window);
        glfwTerminate();

        return false;
    }

    setupCapture();
    seedAnalysis(startup);
    setupView(startup);
    createProjectionInstances();
    createScopeTextures();

    m_shortcuts = startup.shortcuts;
    m_scopeShortcuts = startup.scopeShortcuts;

    m_worker.start();
    warmFaceDetection();

    observeSystemWake([this] { m_captureController.markStale(); });
    observeEscapeWithoutKeyWindow([this] { m_orphanEscape.store(true); });
    // A foreground switch reroutes the borders at the top of the next frame:
    // the flag makes the loop route on arrival, and the empty event wakes an
    // idle wait so "the next frame" is now.
    observeForegroundChanges([this] {
        m_callbackState.foregroundChanged.store(true);
        glfwPostEmptyEvent();
    });
    rememberApplicationWindow(m_graphics->nativeWindowHandle());
    m_ownPid = ownApplicationPid();

    m_lastActivity = glfwGetTime();
    syncRegionBorder();

    return true;
}

void App::run()
{
    while (!glfwWindowShouldClose(m_window)) {
        runFrame();
    }
}

void App::shutdown()
{
    // No new face-lock probe or display face scan starts once the loop is
    // done, so drain any still in flight before their targets leave scope:
    // the detached threads hold pointers into this object.
    for (;;) {
        bool waiting = m_faceLockProbe.running.load();
        for (const std::unique_ptr<DisplayFaceScan>& scan : m_displayFaceScans) {
            waiting = waiting || scan->running.load();
        }
        if (!waiting) {
            break;
        }
        std::this_thread::yield();
    }

    persistPreferences();
    unwatchWindowMotion();
    // The observer reaches this object and posts GLFW events, so it must not
    // outlive either.
    unobserveForegroundChanges();
    hideAttachedEditDim();
    hideRegionBorder();
    m_worker.stop();
    m_capture->stop();
    m_graphics->shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool App::createMainWindow(const Preferences& startup)
{
    m_graphics = createGraphicsBackend();
    m_graphics->setWindowHints();
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    // On Windows the window follows its monitor's scale, so it keeps its
    // physical size when dragged between differently scaled monitors; macOS
    // ignores the hint (scaling lives in the framebuffer there).
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    // Hidden until the saved placement is applied: geometry settles before the
    // first paint, and no intermediate rectangle flashes.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    m_window = glfwCreateWindow(startup.windowWidth, startup.windowHeight, "SideScopes", nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();

        return false;
    }
    // The escape hatch for GLFW's non-capturing C callbacks: they recover the
    // state through this pointer.
    glfwSetWindowUserPointer(m_window, &m_callbackState);
    restoreWindowPlacement(m_window, startup);
    glfwShowWindow(m_window);
    // A development build wears its version in the title bar; a release keeps
    // the plain name.
    if (m_versionInfo.development) {
        glfwSetWindowTitle(m_window, ("SideScopes " + m_versionInfo.display).c_str());
    }
    glfwSetWindowIconifyCallback(m_window, [](GLFWwindow* iconifyTarget, int) {
        static_cast<AppCallbackState*>(glfwGetWindowUserPointer(iconifyTarget))->iconifyChanged.store(true);
    });

    return true;
}

void App::setupImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is ours to persist
    ImGui::StyleColorsDark();
    applyTheme();
    m_callbackState.monospaceFont = loadInterfaceFont(m_window);
    refreshUiScale();
}

void App::applyUiScale(float scale)
{
    m_uiScale = scale;
    // Scaling an already scaled style would compound, so rebuild from the base
    // theme each time.
    applyTheme();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
}

bool App::refreshUiScale()
{
    // The OS scale is the baseline - it already carries the monitor's own
    // recommendation - and the user factor asks for more or less on top. Folding
    // them in one place keeps the startup and monitor-change sites in step, so a
    // window crossing displays never loses the preference.
    const float target = computeUiScale(m_window) * m_userUiScaleFactor;
    if (target == m_uiScale) {
        return false;
    }
    applyUiScale(target);

    return true;
}

void App::setupCapture()
{
    // The source and controller are constructed with the App; here they only
    // start capturing. The display under this window's center: full-screen
    // capture is a promise about the screen the user can see the scopes on.
    if (m_captureController.requestPermission()) {
        m_captureController.requestDisplay(displayOfWindow().value_or(0));
        m_captureController.start();
    }
}

void App::seedAnalysis(const Preferences& startup)
{
    // The worker is driven entirely by scope id: each scope's saved parameters
    // fan out by key straight from the preference map into the module's
    // declarative vocabulary. Smoothing rides the same map but belongs to the
    // host, so it is filtered out. The parade shares the waveform's gain and
    // stride, so both are re-seeded from the waveform.
    for (const auto& [id, params] : startup.scopeParams) {
        for (const auto& [key, value] : params) {
            if (key != "smoothing_ms") {
                m_analysis.scopeParams[id][key] = value;
            }
        }
    }
    m_analysis.scopeParams[ParadeScopeId]["gain"] = m_analysis.scopeParams[WaveformScopeId]["gain"];
    m_analysis.scopeParams[ParadeScopeId]["stride"] = m_analysis.scopeParams[WaveformScopeId]["stride"];
    m_analysis.imageSizes[VectorscopeScopeId] = {DefaultVectorscopeSize, DefaultVectorscopeSize};
    m_analysis.imageSizes[WaveformScopeId] = {DefaultWaveformColumns, WaveformLevels};
    m_analysis.imageSizes[ParadeScopeId] = {DefaultWaveformColumns, WaveformLevels};
    m_analysis.imageSizes[HistogramScopeId] = {Histogram::ImageWidth, Histogram::Height};
}

void App::setupView(const Preferences& startup)
{
    // The file format caps its pin list at the ring's capacity; the two
    // constants sit in different layers, so the build checks they agree.
    static_assert(MaximumPins == PinBoard::Maximum);
    m_pins.restore(startup.pins, startup.pinComparator);
    m_view.restoreStack(startup.scopeStack);
    m_view.setGraticule(startup.showGraticule);
    m_view.setZoom(startup.vectorscopeZoom);
    m_view.setOrientation(orientationFromInt(startup.layoutOrientation));
    m_view.setWeights(startup.layoutWeights);
    m_layoutPresets = startup.layoutPresets;
    m_activePresetSlot = startup.layoutActiveSlot;
    // The stored factor is cleaned to an offered step here, at the app boundary,
    // so core preferences never depend on the app's scaling policy. setupImGui
    // already applied the OS scale at the 1.0 default; fold the preference in now,
    // before the first frame.
    m_userUiScaleFactor = cleanedUiScaleFactor(startup.uiScaleFactor);
    refreshUiScale();
    // The intensity control is derived from each trace's saved gain; smoothing
    // is the host's own per-scope value, read straight from the preferences.
    const auto startupSmoothing = [&](std::string_view id, double fallback) -> float {
        const auto scope = startup.scopeParams.find(std::string{id});
        if (scope == startup.scopeParams.end()) {
            return static_cast<float>(fallback);
        }
        const auto value = scope->second.find("smoothing_ms");

        return value != scope->second.end() ? static_cast<float>(value->second) : static_cast<float>(fallback);
    };
    m_view.setIntensity(VectorscopeScopeId,
                        intensityFromTraceGain(static_cast<float>(scopeParam(VectorscopeScopeId, "gain", 3.0)),
                                               VectorscopeIntensityShift));
    m_view.setIntensity(WaveformScopeId,
                        intensityFromTraceGain(static_cast<float>(scopeParam(WaveformScopeId, "gain", 0.05))));
    m_view.setSmoothing(VectorscopeScopeId, startupSmoothing(VectorscopeScopeId, 75.0));
    m_view.setSmoothing(WaveformScopeId, startupSmoothing(WaveformScopeId, 100.0));
    m_analysis.enabledScopes = m_view.enabledScopeIds();
}

void App::createProjectionInstances()
{
    // Projection instances place the overlays and markers on the main thread:
    // one module instance per scope, drawing declarative graticule primitives
    // and cursor markers. They never accumulate. The color picker has no module
    // instance, so it is skipped here and drawn as host state.
    for (const HostScope& scope : m_scopeRegistry.scopes()) {
        if (scope.descriptor != nullptr) {
            m_projectionInstances.emplace(scope.id, builtinModules().createInstance(scope.id));
        }
    }
}

std::unique_ptr<ScopeTexture> App::createBlankTexture(int width, int height)
{
    auto texture = m_graphics->createScopeTexture(width, height);
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

void App::createScopeTextures()
{
    // One texture per module scope, keyed by id and sized from its descriptor;
    // the upload path resizes to whatever the worker actually produces. The
    // color picker has no descriptor and draws no texture.
    for (const HostScope& scope : m_scopeRegistry.scopes()) {
        if (!scope.descriptor) {
            continue;
        }
        const int width = scope.descriptor->image_width > 0 ? scope.descriptor->image_width : DefaultVectorscopeSize;
        const int height = scope.descriptor->image_height > 0 ? scope.descriptor->image_height : DefaultVectorscopeSize;
        m_scopeTextures[scope.id] = createBlankTexture(width, height);
    }
    m_panePoints.assign(m_scopeRegistry.scopes().size(), ImVec2());
    for (std::size_t i = 0; i < m_scopeRegistry.scopes().size(); ++i) {
        m_paneIds.push_back("##pane" + std::to_string(i));
        m_dividerIds.push_back("##divider" + std::to_string(i));
    }
}

std::optional<uint32_t> App::displayOfWindow() const
{
    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(m_window, &windowX, &windowY);
    glfwGetWindowSize(m_window, &windowWidth, &windowHeight);

    return displayAtPoint(DesktopPoint{windowX + windowWidth / 2.0, windowY + windowHeight / 2.0});
}

double App::scopeParam(std::string_view id, std::string_view key, double fallback) const
{
    const auto scope = m_analysis.scopeParams.find(std::string{id});
    if (scope == m_analysis.scopeParams.end()) {
        return fallback;
    }
    const auto value = scope->second.find(std::string{key});

    return value != scope->second.end() ? value->second : fallback;
}

std::pair<int, int> App::currentSize(std::string_view id) const
{
    const auto at = m_analysis.imageSizes.find(std::string{id});

    return at != m_analysis.imageSizes.end() ? at->second : std::pair<int, int>{0, 0};
}

HistogramStyle App::currentHistogramStyle() const
{
    return scopeParam(HistogramScopeId, "style", 0.0) < 0.5 ? HistogramStyle::PerChannel : HistogramStyle::Combined;
}

void App::setWaveformGain(double gain)
{
    m_analysis.scopeParams[WaveformScopeId]["gain"] = gain;
    m_analysis.scopeParams[ParadeScopeId]["gain"] = gain;
}

const SsScopeDescriptor* App::descriptorFor(std::string_view id) const
{
    const HostScope* hostScope = m_scopeRegistry.byId(id);

    return hostScope != nullptr ? hostScope->descriptor : nullptr;
}

void App::configureProjectionInstances()
{
    // Reconfigures every projection instance from the current settings, through
    // the same assembleScopeParams the worker uses, so an overlay can never
    // disagree with its trace.
    for (auto& [id, instance] : m_projectionInstances) {
        const SsScopeDescriptor* descriptor = descriptorFor(id);
        std::vector<SsParamValue> values;
        const auto params = m_analysis.scopeParams.find(id);
        if (params != m_analysis.scopeParams.end() && descriptor != nullptr) {
            values = assembleScopeParams(params->second, *descriptor);
        }
        (void)instance.configure(values);
    }
}

const ScopeInstance* App::projectionFor(std::string_view id) const
{
    const auto at = m_projectionInstances.find(std::string{id});

    return at != m_projectionInstances.end() ? &at->second : nullptr;
}

std::string App::bindingFor(std::string_view id) const
{
    return resolveBinding(m_scopeShortcuts, m_scopeRegistry, id);
}

bool App::pinsAvailable() const
{
    // Pins mark any scope that declares itself a pin target (plus the host's
    // own color picker); without one on screen, the tool's button, menu
    // entries, and shortcuts all stand down together.
    for (const std::string& scopeId : m_view.stack()) {
        if (scopeId == ColorPickerScopeId) {
            return true;
        }
        const HostScope* hostScope = m_scopeRegistry.byId(scopeId);
        if (hostScope != nullptr && hostScope->descriptor != nullptr &&
            (hostScope->descriptor->flags & SS_SCOPE_PIN_TARGET) != 0u) {
            return true;
        }
    }

    return false;
}

const ScopeImage& App::imageForId(std::string_view id) const
{
    static const ScopeImage empty;
    const auto at = m_output.images.find(std::string{id});

    return at != m_output.images.end() ? at->second : empty;
}

ScopeTexture& App::textureForId(std::string_view id)
{
    return *m_scopeTextures.at(std::string{id});
}

void App::uploadScope(std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image)
{
    if (image.width <= 0 || image.height <= 0) {
        return;
    }
    if (image.rgba.size() < static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        return;
    }
    if (texture->width() != image.width || texture->height() != image.height) {
        texture = m_graphics->createScopeTexture(image.width, image.height);
    }
    texture->upload(image);
}

void App::uploadVisibleScopes()
{
    for (const std::string& id : m_view.stack()) {
        const auto texture = m_scopeTextures.find(id);
        if (texture == m_scopeTextures.end()) {
            continue;  // the color picker has no texture
        }
        uploadScope(texture->second, imageForId(id));
    }
}

void App::refreshActivatedScope(std::string_view id)
{
    // A scope draws the same frame it turns on, but the worker only computes
    // what is enabled, so a newly shown scope's image is stale. Turning it on
    // pushes the settings immediately and waits briefly for the recompute; on
    // timeout the stale image stands in until the recompute lands a frame later.
    if (m_scopeTextures.find(std::string{id}) == m_scopeTextures.end()) {
        return;  // the color picker asks nothing of the worker
    }
    const uint64_t staleSequence = imageForId(id).sequence;
    m_worker.updateSettings(m_analysis);
    const double deadline = glfwGetTime() + 0.08;
    while (glfwGetTime() < deadline) {
        if (m_worker.fetchOutput(m_outputVersion, m_output) && imageForId(id).sequence != staleSequence &&
            imageForId(id).width > 0) {
            uploadVisibleScopes();

            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    uploadVisibleScopes();  // timeout: a stale image beats none
}

void App::toggleScope(std::string_view id)
{
    const bool activated = m_view.toggle(id);
    m_analysis.enabledScopes = m_view.enabledScopeIds();
    if (activated) {
        refreshActivatedScope(id);
    }
    m_analysisDirty = true;
}

void App::chooseScope(std::string_view id, bool stack)
{
    const bool activated = m_view.choose(id, stack);
    m_analysis.enabledScopes = m_view.enabledScopeIds();
    if (activated) {
        refreshActivatedScope(id);
    }
    m_analysisDirty = true;
}

bool App::isFullScreen() const
{
    return m_analysis.region.leftPercent <= 0.0 && m_analysis.region.topPercent <= 0.0 &&
           m_analysis.region.rightPercent >= 100.0 && m_analysis.region.bottomPercent >= 100.0;
}

RegionKind App::regionKind() const
{
    // The active identity IS the kind: the follow step takes the attached
    // region exactly while a visible attached window holds the focus, and
    // falls back to the global region the moment it does not.
    return m_activeWindowIdentity != 0 ? RegionKind::Attached : RegionKind::Global;
}

void App::syncRegionBorder()
{
    if (m_captureController.capturedDisplay() == 0) {
        return;
    }
    // The border shows only while this application is itself visible - a
    // hidden or minimized SideScopes must not leave regions floating on
    // screen - never during a pick or window motion. What it outlines
    // follows the focus routing already folded into the analysis region: the
    // attached region on the focused attached window (label and warm dress),
    // else the plain global one. Called every frame; the platform side makes
    // the unchanged case free.
    if (m_regionPicking || isFullScreen() || applicationHidden() || m_attachedWindowMoving || m_faceLockHunting ||
        regionContentUnsettled() || glfwGetWindowAttrib(m_window, GLFW_ICONIFIED)) {
        hideRegionBorder();
    } else {
        const bool attached = regionKind() == RegionKind::Attached;
        if (!attached && m_captureController.capturedDisplay() != m_displayLabelId) {
            m_displayLabelId = m_captureController.capturedDisplay();
            m_displayLabel = borderLabelFrom(displayName(m_displayLabelId), "Display");
        }
        showRegionBorder(m_captureController.capturedDisplay(), m_analysis.region,
                         attached ? m_attachActiveLabel : m_displayLabel, attached);
    }
}

// Sets the capture region, skipping a no-op so the worker and border are not
// nudged for nothing.
void App::setRegion(const RegionOfInterest& region)
{
    if (region.leftPercent == m_analysis.region.leftPercent && region.topPercent == m_analysis.region.topPercent &&
        region.rightPercent == m_analysis.region.rightPercent &&
        region.bottomPercent == m_analysis.region.bottomPercent) {
        return;
    }

    m_analysis.region = region;
    m_analysisDirty = true;
    m_lastActivity = glfwGetTime();
}

void App::resetToFullScreen()
{
    // Resets all selection: a pending pick, every attached window, and the
    // global region alike. The border sync rides the analysis-dirty path.
    cancelRegionPick();
    if (m_attach.attached()) {
        m_attach.detachAll();
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
    m_globalRegion = RegionOfInterest{};
    m_analysis.region = RegionOfInterest{};
    m_analysisDirty = true;
}

void App::persistPreferences()
{
    Preferences preferences;
    // The worker's parameter map is the persisted state directly; only the
    // host-owned smoothing control is folded back in. The parade is dropped: it
    // mirrors the waveform and re-seeds on load.
    preferences.scopeParams = m_analysis.scopeParams;
    preferences.scopeParams.erase(ParadeScopeId);
    preferences.scopeParams[VectorscopeScopeId]["smoothing_ms"] = m_view.smoothing(VectorscopeScopeId);
    preferences.scopeParams[WaveformScopeId]["smoothing_ms"] = m_view.smoothing(WaveformScopeId);
    preferences.scopeStack = m_view.stackTokens();
    preferences.showGraticule = m_view.graticule();
    preferences.vectorscopeZoom = m_view.zoom();
    preferences.layoutOrientation = orientationToInt(m_view.orientation());
    preferences.layoutWeights = m_view.weightsSnapshot();
    preferences.layoutPresets = m_layoutPresets;
    preferences.layoutActiveSlot = m_activePresetSlot;
    preferences.uiScaleFactor = m_userUiScaleFactor;
    preferences.shortcuts = m_shortcuts;
    preferences.scopeShortcuts = m_scopeShortcuts;
    preferences.pins = m_pins.colors();
    preferences.pinComparator = m_pins.comparator();
    glfwGetWindowPos(m_window, &preferences.windowX, &preferences.windowY);
    glfwGetWindowSize(m_window, &preferences.windowWidth, &preferences.windowHeight);
    if (!savePreferences(preferences, preferencesFilePath())) {
        std::fprintf(stderr, "sidescopes: failed to save preferences to %s\n", preferencesFilePath().c_str());
    }
}

void App::runFrame()
{
    // Frame-scoped signals start clear each iteration, exactly as fresh locals
    // would; the phase methods below fill them in.
    m_vectorscopeColor.reset();
    m_waveformColor.reset();
    m_wantRegionPick.reset();

    pumpEvents();
    markFrameBodyStart();
    drainAsyncSignals();
    // Capture is a service that dies (lock screen, display sleep); restarting
    // it is our job.
    m_captureController.service(glfwGetTime());
    // Attached regions: observe the attached windows and route the analysis by
    // the focused window. The border reconciles here every frame in both
    // regimes, so no missed edge can strand it on screen.
    followAttachedWindow();
    syncRegionBorder();
    followWindowDisplay();
    syncUiScaleToMonitor();

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    if (framebufferWidth == 0 || framebufferHeight == 0) {
        return;
    }
    if (!m_graphics->beginFrame(framebufferWidth, framebufferHeight)) {
        return;
    }

    if (m_worker.fetchOutput(m_outputVersion, m_output)) {
        uploadVisibleScopes();
        SS_DIAG(Perf, "pass analysis_ms=%.1f", m_output.accumulateMilliseconds);
        m_lastActivity = glfwGetTime();
    }
    m_frameSize = m_worker.latestFrameSize();
    publishSelfWindowMask();
    sampleCursorColor();
    updateAdaptiveDetail(framebufferWidth);

    drawFrameUi();

    ImGui::Render();
    presentFrame();

    // Second follow step, right after the vsync wait: the pre-frame geometry
    // is a frame stale by now, and a border moved from it would trail a
    // fast-dragged window visibly.
    followAttachedWindow();

    // The blocking overlay runs after the frame is submitted; capture and
    // analysis keep flowing underneath.
    handleRegionPicking();
    handleRegionBorderEdit();
    pollActiveRegionPick();
    commitAnalysisChanges();
}

void App::markFrameBodyStart()
{
    if (diagEnabled(DiagChannel::Perf)) {
        m_frameBodyStart = std::chrono::steady_clock::now();
        m_frameBodyStamped = true;
    }
}

void App::presentFrame()
{
    // Off: a plain present. On: bracket the platform present so the frame
    // body (build, since markFrameBodyStart) and the present/vsync wait fall
    // on one line. On Windows the wait is DwmFlush's compositor tick; on
    // macOS it is the drawable present submit, the swap seam in shared code.
    // The record toggle lands mid-frame (inside the UI pass), so the first
    // present after enabling has no stamped body start - that frame logs
    // nothing rather than timing from a stale stamp.
    const bool bodyStamped = m_frameBodyStamped;
    m_frameBodyStamped = false;
    if (!diagEnabled(DiagChannel::Perf)) {
        m_graphics->endFrame();

        return;
    }
    const auto presentStart = std::chrono::steady_clock::now();
    m_graphics->endFrame();
    if (!bodyStamped) {
        return;
    }
    const auto frameEnd = std::chrono::steady_clock::now();
    const double bodyMs = std::chrono::duration<double, std::milli>(presentStart - m_frameBodyStart).count();
    const double presentMs = std::chrono::duration<double, std::milli>(frameEnd - presentStart).count();
    SS_DIAG(Perf, "frame body_ms=%.1f present_ms=%.1f", bodyMs, presentMs);
}

void App::pumpEvents()
{
    // Idle: with no new output, no cursor motion, and no interaction, wait for
    // events at a slow tick instead of spinning at refresh - in short slices
    // while windows are attached, so their motion and focus stay fresh.
    if (glfwGetTime() - m_lastActivity > 0.5) {
        if (m_attach.attached() && !m_regionPicking) {
            idleWaitWatchingAttachedWindow();
        } else {
            glfwWaitEventsTimeout(0.1);
        }
    } else {
        glfwPollEvents();
    }
}

void App::drainAsyncSignals()
{
    // First of the drains, and ahead of the capture service below: the focus
    // routing is what takes a stale border down, and everything after this
    // point can stall the tick - a capture restart most of all.
    if (m_callbackState.foregroundChanged.exchange(false)) {
        SS_DIAG(Attach, "fg-event wake");
        followAttachedWindow();
        m_lastActivity = glfwGetTime();
    }
    drainDisplayFaceScans();
    if (m_callbackState.iconifyChanged.exchange(false)) {
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
    if (m_orphanEscape.exchange(false)) {
        resetToFullScreen();
        m_lastActivity = glfwGetTime();
    }
    // Keys the border panel took while it held the keyboard: Escape and the
    // shortcuts keep working right after a border interaction. Escape on the
    // border dismisses only the region it outlines - like its close button -
    // while Escape in the main window stays the full reset.
    for (const BorderKeyPress& press : drainBorderKeyPresses()) {
        if (press.escape) {
            dismissEditedBorder();
        } else {
            triggerShortcut(press.key, press.shift);
        }
        m_lastActivity = glfwGetTime();
    }
}

void App::followWindowDisplay()
{
    // With no region drawn and no window attached, capture follows the display
    // this window sits on. A drawn region or an attached window pins capture to
    // its own display regardless of the window.
    if (m_captureController.permissionGranted() && !m_captureController.dead() && !m_regionPicking && isFullScreen() &&
        !m_attach.attached()) {
        const auto homeDisplay = displayOfWindow();
        if (homeDisplay && *homeDisplay != m_captureController.capturedDisplay()) {
            m_captureController.requestDisplay(*homeDisplay);
            if (m_captureController.start()) {
                m_lastActivity = glfwGetTime();
            }
        }
    }
}

void App::syncUiScaleToMonitor()
{
    // The window may have moved to a monitor with a different scale; the user
    // factor rides along through refreshUiScale.
    if (refreshUiScale()) {
        m_lastActivity = glfwGetTime();
    }
}

void App::publishSelfWindowMask()
{
    // Publish our own window rectangle (frame pixels, generous chrome margins)
    // so analysis masks it out of change detection.
    if (!m_frameSize || m_captureController.capturedDisplay() == 0) {
        return;
    }
    const auto geometry = geometryOfDisplay(m_captureController.capturedDisplay());
    if (!geometry) {
        return;
    }
    int windowX = 0, windowY = 0, windowW = 0, windowH = 0;
    glfwGetWindowPos(m_window, &windowX, &windowY);
    glfwGetWindowSize(m_window, &windowW, &windowH);
    const double scaleX = m_frameSize->width / geometry->widthPoints;
    const double scaleY = m_frameSize->height / geometry->heightPoints;
    // The chrome margins are 100%-scale units like the rest of the interface,
    // so they grow with the monitor's scale.
    const IntRect selfWindow{static_cast<int>((windowX - geometry->originX - 8.0f * m_uiScale) * scaleX),
                             static_cast<int>((windowY - geometry->originY - 42.0f * m_uiScale) * scaleY),
                             static_cast<int>((static_cast<float>(windowW) + 16.0f * m_uiScale) * scaleX),
                             static_cast<int>((static_cast<float>(windowH) + 58.0f * m_uiScale) * scaleY)};
    if (selfWindow.x != m_analysis.maskedWindow.x || selfWindow.y != m_analysis.maskedWindow.y ||
        selfWindow.width != m_analysis.maskedWindow.width || selfWindow.height != m_analysis.maskedWindow.height) {
        m_analysis.maskedWindow = selfWindow;
        m_analysisDirty = true;
    }
}

void App::sampleCursorColor()
{
    // Cursor color, smoothed per scope with its own rhythm. On the captured
    // display it reads the capture stream's frame; on every other display a
    // throttled one-shot sample keeps the readout alive even while capture is
    // paused.
    if (m_captureController.capturedDisplay() == 0) {
        return;
    }
    const auto cursor = globalCursorPosition();
    if (!cursor) {
        return;
    }
    if (std::abs(cursor->x - m_lastCursor.x) + std::abs(cursor->y - m_lastCursor.y) > 0.5) {
        m_lastCursor = *cursor;
        m_lastActivity = glfwGetTime();
    }
    std::optional<FloatColor> sampled;
    const bool onCapturedDisplay = displayAtPoint(*cursor).value_or(0) == m_captureController.capturedDisplay();
    if (onCapturedDisplay && !m_captureController.dead() && m_frameSize) {
        if (const auto geometry = geometryOfDisplay(m_captureController.capturedDisplay())) {
            const int pixelX =
                static_cast<int>((cursor->x - geometry->originX) * m_frameSize->width / geometry->widthPoints);
            const int pixelY =
                static_cast<int>((cursor->y - geometry->originY) * m_frameSize->height / geometry->heightPoints);
            sampled = m_worker.sampleFrameColor(pixelX, pixelY);
        }
    } else {
        if (glfwGetTime() > m_nextScreenSample) {
            m_nextScreenSample = glfwGetTime() + 0.05;
            auto screenSample = m_screenSample;
            sampleScreenColorAsync(*cursor, [screenSample](std::optional<FloatColor> color) {
                if (!color) {
                    return;
                }
                std::lock_guard lock(screenSample->mutex);
                screenSample->color = color;
            });
        }
        std::lock_guard lock(m_screenSample->mutex);
        sampled = m_screenSample->color;
    }
    if (sampled) {
        m_vectorscopeMarker.setTimeConstant(m_view.smoothing(VectorscopeScopeId));
        m_waveformMarker.setTimeConstant(m_view.smoothing(WaveformScopeId));
        const ImGuiIO& io = ImGui::GetIO();
        m_vectorscopeColor = m_vectorscopeMarker.update(*sampled, io.DeltaTime);
        m_waveformColor = m_waveformMarker.update(*sampled, io.DeltaTime);
    }
}

ImVec2 App::paneSizePixels(std::string_view id, float density) const
{
    const ImVec2& points = m_panePoints[static_cast<std::size_t>(m_scopeRegistry.indexOf(id))];

    return ImVec2(points.x * density, points.y * density);
}

std::pair<int, int> App::desiredWaveformSize(float density, int regionWidth) const
{
    const std::pair<int, int> waveSize = currentSize(WaveformScopeId);
    int wantColumns = waveSize.first;
    int wantHeight = waveSize.second;
    if (m_view.shows(WaveformScopeId) || m_view.shows(ParadeScopeId)) {
        const float wfWidth =
            std::max(paneSizePixels(WaveformScopeId, density).x, paneSizePixels(ParadeScopeId, density).x);
        const float wfHeight =
            std::max(paneSizePixels(WaveformScopeId, density).y, paneSizePixels(ParadeScopeId, density).y);
        wantColumns = wfWidth >= 1400.0f ? 2048 : wfWidth >= 500.0f ? 1024 : 512;
        if (regionWidth > 0) {
            wantColumns = std::min(wantColumns, regionWidth >= 2048 ? 2048 : regionWidth >= 1024 ? 1024 : 512);
        }
        wantHeight = wfHeight >= 560.0f ? 512 : WaveformLevels;
    }

    return {wantColumns, wantHeight};
}

std::pair<int, int> App::desiredHistogramSize(float density) const
{
    const std::pair<int, int> histSize = currentSize(HistogramScopeId);
    int wantHistWidth = histSize.first;
    int wantHistHeight = histSize.second;
    if (m_view.shows(HistogramScopeId)) {
        // Near one texture pixel per screen pixel keeps the outline's width even
        // on flats and steep slopes alike.
        const ImVec2 scopePane = paneSizePixels(HistogramScopeId, density);
        wantHistWidth = scopePane.x >= 1400.0f ? 2048 : scopePane.x >= 500.0f ? 1024 : 512;
        wantHistHeight = scopePane.y >= 560.0f ? 768 : 384;
    }

    return {wantHistWidth, wantHistHeight};
}

int App::desiredVectorscopeSize(float density) const
{
    int wantVectorscope = currentSize(VectorscopeScopeId).second;
    if (m_view.shows(VectorscopeScopeId)) {
        // Purely a display resolution: accumulation stays on the 256-code grid
        // and a finer image is interpolated from it, so a sparse region costs
        // nothing extra.
        const ImVec2 scopePane = paneSizePixels(VectorscopeScopeId, density);
        const float extent = std::min(scopePane.x, scopePane.y);
        wantVectorscope = extent >= 480.0f ? 512 : 256;
    }

    return wantVectorscope;
}

void App::updateAdaptiveDetail(int framebufferWidth)
{
    // Resolution follows the pane a scope actually gets, and never exceeds what
    // the region can populate; desired resolutions are debounced so a live
    // resize does not thrash engine reallocation.
    int windowW = 0;
    int windowH = 0;
    glfwGetWindowSize(m_window, &windowW, &windowH);
    const float density = windowW > 0 ? static_cast<float>(framebufferWidth) / static_cast<float>(windowW) : 1.0f;
    int regionWidth = 0;
    if (m_frameSize) {
        regionWidth = m_analysis.region.toPixels(m_frameSize->width, m_frameSize->height).width;
    }

    const std::pair<int, int> waveSize = currentSize(WaveformScopeId);
    const std::pair<int, int> histSize = currentSize(HistogramScopeId);
    const std::pair<int, int> vecSize = currentSize(VectorscopeScopeId);
    const auto [wantColumns, wantHeight] = desiredWaveformSize(density, regionWidth);
    const auto [wantHistWidth, wantHistHeight] = desiredHistogramSize(density);
    const int wantVectorscope = desiredVectorscopeSize(density);

    const bool differs = wantColumns != waveSize.first || wantHeight != waveSize.second ||
                         wantVectorscope != vecSize.second || wantHistWidth != histSize.first ||
                         wantHistHeight != histSize.second;
    if (!differs) {
        m_pendingColumns = 0;
    } else if (m_pendingColumns != wantColumns || m_pendingImageHeight != wantHeight ||
               m_pendingVectorscope != wantVectorscope || m_pendingHistWidth != wantHistWidth ||
               m_pendingHistHeight != wantHistHeight) {
        m_pendingColumns = wantColumns;
        m_pendingImageHeight = wantHeight;
        m_pendingVectorscope = wantVectorscope;
        m_pendingHistWidth = wantHistWidth;
        m_pendingHistHeight = wantHistHeight;
        m_detailPendingSince = glfwGetTime();
    } else if (glfwGetTime() - m_detailPendingSince > 0.4) {
        m_analysis.imageSizes[WaveformScopeId] = {wantColumns, wantHeight};
        m_analysis.imageSizes[ParadeScopeId] = {wantColumns, wantHeight};
        m_analysis.imageSizes[VectorscopeScopeId] = {wantVectorscope, wantVectorscope};
        m_analysis.imageSizes[HistogramScopeId] = {wantHistWidth, wantHistHeight};
        m_analysisDirty = true;
    }
}

void App::drawFrameUi()
{
    ImGui::NewFrame();
    beginHostWindow();

    // The stacking modifier reads the OS's live key state, not the event-tracked
    // one: a Shift key-up swallowed by a system overlay leaves the cache stuck
    // exactly when the user next switches a scope.
    const ModifierState modifiers = currentModifiers();
    drawScopeToggles(modifiers.shift);
    handleShortcuts(modifiers);
    drawRegionToolIcons();
    drawScopePanes();
    drawStatusBar();
    handleContextMenu();

    ImGui::End();
    ImGui::PopStyleVar();

    const SettingsContext settingsCtx{m_showSettings,  m_view,   m_analysis,    m_analysisDirty,
                                      m_scopeRegistry, m_output, m_versionInfo, m_captureController.status()};
    drawSettingsWindow(settingsCtx);
    drawAboutWindow();

    if (ImGui::IsAnyItemActive()) {
        m_lastActivity = glfwGetTime();
        m_nextPreferencesSave = glfwGetTime() + 1.0;
    }
}

void App::beginHostWindow()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::Begin("##host", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
}

void App::drawScopeToggles(bool stackModifier)
{
    // Scope toggles are letter chips; switching is the common case, so a plain
    // click shows one scope alone and Shift stacks.
    char tooltip[96];
    const auto scopeTooltip = [&](const char* name, const std::string& binding, const char* extra) {
        std::snprintf(tooltip, sizeof(tooltip), "%s - %s to switch, Shift+%s to stack%s", name, binding.c_str(),
                      binding.c_str(), extra);
        return tooltip;
    };
    drawPresetPicker();
    ImGui::SameLine(0.0f, 8.0f);
    for (const HostScope& scope : m_scopeRegistry.scopes()) {
        if (scope.letter == 0) {
            continue;
        }
        const ScopeChrome chrome = scopeChromeFor(scope.id);
        const char letter[2] = {scope.letter, '\0'};
        if (scopeToggleButton(chrome.buttonId, letter, m_view.shows(scope.id),
                              scopeTooltip(chrome.name, bindingFor(scope.id), chrome.extra))) {
            chooseScope(scope.id, stackModifier);
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::SameLine(0.0f, 8.0f);
}

void App::handleShortcuts(const ModifierState& modifiers)
{
    // Command, Control, and Option chords belong to the system and the window,
    // so any of them silences the plain-letter shortcuts. Shift alone stays
    // meaningful: it stacks.
    const bool systemChord = modifiers.command || modifiers.control || modifiers.option;
    handleCommandChords(modifiers);
    handleControlChords(modifiers);
    handleLetterShortcuts(modifiers, systemChord);
}

void App::handleCommandChords(const ModifierState& modifiers)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (platformHidesWindowOnCommandW() && modifiers.command && !modifiers.control && !modifiers.option &&
        !io.WantTextInput) {
        // Cmd+W dismisses through the system hide - the exact machinery behind
        // Cmd+H, so the Dock click or Cmd+Tab restores every window natively,
        // the border included.
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            hideApplication();
        }
        // Cmd+comma opens settings everywhere on macOS.
        if (ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
            m_showSettings = true;
        }
    }
}

void App::handleControlChords(const ModifierState& modifiers)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (platformMinimizesWindowOnControlW() && modifiers.control && !modifiers.command && !modifiers.option &&
        !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_W, false)) {
        glfwIconifyWindow(m_window);
    }
    if (platformQuitsOnControlQ() && modifiers.control && !modifiers.command && !modifiers.option &&
        !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
    }
}

void App::handleLetterShortcuts(const ModifierState& modifiers, bool systemChord)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || systemChord) {
        return;
    }
    // Each scope's toggle key is resolved by id; a letterless scope has an empty
    // binding, which never matches a press.
    for (const HostScope& scope : m_scopeRegistry.scopes()) {
        if (shortcutPressed(bindingFor(scope.id))) {
            chooseScope(scope.id, modifiers.shift);
        }
    }
    for (const std::string& binding :
         {m_shortcuts.attachWindow, m_shortcuts.drawRegion, m_shortcuts.attachFace, m_shortcuts.pinColor}) {
        if (shortcutPressed(binding)) {
            triggerShortcut(binding, modifiers.shift);
        }
    }
    handleViewShortcuts();
    handlePresetShortcuts(modifiers);
}

// The single map from a shortcut key to its action, shared by the ImGui
// press detection above and the border panel's forwarded keys.
// @return Whether the key matched anything.
bool App::triggerShortcut(const std::string& key, bool shift)
{
    for (const HostScope& scope : m_scopeRegistry.scopes()) {
        if (!bindingFor(scope.id).empty() && bindingFor(scope.id) == key) {
            chooseScope(scope.id, shift);

            return true;
        }
    }
    if (key == m_shortcuts.attachWindow) {
        m_wantRegionPick = RegionPickerMode::AttachWindow;
    } else if (key == m_shortcuts.drawRegion) {
        m_wantRegionPick = RegionPickerMode::DrawGlobal;
    } else if (key == m_shortcuts.attachFace && supportsFaceDetection()) {
        m_wantRegionPick = RegionPickerMode::AttachFace;
    } else if (key == m_shortcuts.pinColor && pinsAvailable()) {
        // One pin tool; each click inside decides between pin-and-close and
        // Shift's pin-and-continue.
        m_wantRegionPick = RegionPickerMode::PinColor;
    } else if (key == m_shortcuts.vectorscopeZoom) {
        m_view.setZoom(m_view.zoom() >= 4 ? 1 : m_view.zoom() * 2);
    } else if (key == m_shortcuts.fullScreen) {
        resetToFullScreen();
    } else {
        return false;
    }

    return true;
}

void App::handlePresetShortcuts(const ModifierState& modifiers)
{
    // Digit N loads preset slot N; Shift+N saves the current layout into it. The
    // guard against text input and system chords is the caller's, shared with
    // the letter shortcuts.
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const auto key = static_cast<ImGuiKey>(ImGuiKey_0 + slot);
        if (!ImGui::IsKeyPressed(key, false)) {
            continue;
        }
        if (modifiers.shift) {
            saveLayoutPreset(slot);
        } else {
            loadLayoutPreset(slot);
        }
    }
}

std::map<std::string, double> App::currentStackWeights() const
{
    // A self-contained snapshot: every scope on screen with its current weight,
    // so a loaded preset reproduces the exact split even for scopes left at the
    // default weight.
    std::map<std::string, double> weights;
    for (const std::string& id : m_view.stack()) {
        weights[id] = m_view.weight(id);
    }

    return weights;
}

void App::saveLayoutPreset(int slot)
{
    m_layoutPresets[static_cast<std::size_t>(slot - 1)] = capturePreset();
    m_activePresetSlot = slot;
    setStatus("preset " + std::to_string(slot) + " saved");
    m_nextPreferencesSave = glfwGetTime() + 1.0;
}

LayoutPreset App::capturePreset() const
{
    LayoutPreset preset;
    preset.stack = m_view.stackTokens();
    preset.orientation = orientationToInt(m_view.orientation());
    preset.weights = currentStackWeights();
    preset.styles = currentStackStyles();

    return preset;
}

bool App::activePresetDirty() const
{
    if (m_activePresetSlot == 0) {
        return false;
    }
    const LayoutPreset& stored = m_layoutPresets[static_cast<std::size_t>(m_activePresetSlot - 1)];
    const LayoutPreset live = capturePreset();

    return live.stack != stored.stack || live.orientation != stored.orientation || live.weights != stored.weights ||
           live.styles != stored.styles;
}

void App::drawPresetPicker()
{
    // A chip like the scope letters, leading the row: the label names the
    // active slot (starred once the live layout drifts; "-" when none), and
    // clicking opens the slot list - the mouse mirror of the digit keys.
    const bool dirty = activePresetDirty();
    char preview[8] = "-";
    if (m_activePresetSlot != 0) {
        std::snprintf(preview, sizeof(preview), "%d%s", m_activePresetSlot, dirty ? "*" : "");
    }
    if (scopeToggleButton("##preset-picker", preview, false, "Layout presets - digits load, Shift+digits save")) {
        ImGui::OpenPopup("##preset-popup");
    }
    const ImVec2 chipMin = ImGui::GetItemRectMin();
    const ImVec2 chipMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(chipMin.x, chipMax.y + 2.0f));
    if (ImGui::BeginPopup("##preset-popup")) {
        for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
            const LayoutPreset& preset = m_layoutPresets[static_cast<std::size_t>(slot - 1)];
            if (ImGui::Selectable(presetLabel(slot, preset).c_str(), slot == m_activePresetSlot)) {
                if (ImGui::GetIO().KeyShift) {
                    saveLayoutPreset(slot);
                } else {
                    loadLayoutPreset(slot);
                }
            }
        }
        ImGui::TextDisabled("click loads - Shift+click saves");
        ImGui::EndPopup();
    }
}

std::map<std::string, std::map<std::string, double>> App::currentStackStyles() const
{
    std::map<std::string, std::map<std::string, double>> styles;
    for (const std::string& scopeId : m_view.stack()) {
        const HostScope* hostScope = m_scopeRegistry.byId(scopeId);
        if (hostScope == nullptr || hostScope->descriptor == nullptr) {
            continue;
        }
        const std::map<std::string, double>& params = paramsOf(scopeId);
        for (uint32_t index = 0; index < hostScope->descriptor->param_count; ++index) {
            const SsParamInfo& info = hostScope->descriptor->params[index];
            if (info.kind != SS_PARAM_CHOICE) {
                continue;
            }
            const auto current = params.find(info.key);
            styles[scopeId][info.key] = current != params.end() ? current->second : info.default_value;
        }
    }

    return styles;
}

void App::applyPresetStyles(const std::map<std::string, std::map<std::string, double>>& styles)
{
    for (const auto& [scopeId, params] : styles) {
        const HostScope* hostScope = m_scopeRegistry.byId(scopeId);
        if (hostScope == nullptr || hostScope->descriptor == nullptr) {
            continue;
        }
        for (const auto& [key, value] : params) {
            const SsParamInfo* info = findParam(hostScope->descriptor, key);
            if (info == nullptr || info->kind != SS_PARAM_CHOICE) {
                continue;
            }
            m_analysis.scopeParams[scopeId][key] = std::clamp(value, info->min_value, info->max_value);
        }
    }
}

void App::loadLayoutPreset(int slot)
{
    const LayoutPreset& preset = m_layoutPresets[static_cast<std::size_t>(slot - 1)];
    if (preset.stack.empty()) {
        setStatus("preset " + std::to_string(slot) + " is empty");

        return;
    }
    m_view.restoreStack(preset.stack);
    m_view.setOrientation(orientationFromInt(preset.orientation));
    m_view.setWeights(preset.weights);
    applyPresetStyles(preset.styles);
    m_activePresetSlot = slot;
    m_analysis.enabledScopes = m_view.enabledScopeIds();
    m_analysisDirty = true;
    setStatus("preset " + std::to_string(slot) + " loaded");
}

void App::handleViewShortcuts()
{
    if (shortcutPressed(m_shortcuts.vectorscopeZoom)) {
        m_view.setZoom(m_view.zoom() >= 4 ? 1 : m_view.zoom() * 2);
    }
    if (shortcutPressed(m_shortcuts.fullScreen)) {
        // Escape peels back one layer at a time: the settings window first, the
        // drawn region only when nothing is stacked above it.
        if (m_showSettings) {
            m_showSettings = false;
        } else {
            resetToFullScreen();
        }
    }
}

// Lazily rasterized textures for the embedded icon set, rebuilt when the
// framebuffer scale changes the requested pixel size.
ImTextureID App::iconTextureId(Icon icon, int sizePixels)
{
    IconTexture& slot = m_iconTextures[static_cast<std::size_t>(icon)];
    if (!slot.texture || slot.sizePixels != sizePixels) {
        ScopeImage image;
        image.width = sizePixels;
        image.height = sizePixels;
        image.rgba = rasterizeIcon(icon, sizePixels);
        slot.texture = m_graphics->createScopeTexture(sizePixels, sizePixels);
        slot.texture->upload(image);
        slot.sizePixels = sizePixels;
    }

    return slot.texture->textureId();
}

void App::placeRegionToolbox()
{
    // The brief note after an attached window closed out from under its region
    // stays on the left, by the scopes cluster, clear of the toolbox.
    if (glfwGetTime() < m_attachNoticeUntil) {
        ImGui::TextDisabled("%s", m_attachDetachNotice.c_str());
        ImGui::SameLine(0.0f, 8.0f);
    }
    // The region toolbox is a constant-width cluster: state dims a tool, it
    // never removes one, so the row reflows only when the WINDOW changes -
    // not when the scope stack does. Right-aligned while it shares the row
    // with the scopes; flush left when it wraps to a row of its own. Narrow
    // windows are the tall beside-the-editor shape, which has the height
    // for a second row; wide strips keep one row.
    const int iconCount = 3 + (supportsFaceDetection() ? 1 : 0);
    const float chip = ImGui::GetTextLineHeight() + 12.0f;
    const float width = static_cast<float>(iconCount) * chip + static_cast<float>(iconCount - 1) * 2.0f;
    const float right = ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetCursorPosX() + width + 8.0f > right) {
        ImGui::NewLine();
    } else {
        ImGui::SetCursorPosX(right - width);
    }
}

void App::drawRegionToolIcons()
{
    char tooltip[96];
    std::snprintf(tooltip, sizeof(tooltip), "Draw a region (%s)", m_shortcuts.drawRegion.c_str());
    const int iconPx = iconPixelSize();
    placeRegionToolbox();
    if (iconButton("##draw-region", iconTextureId(Icon::Pencil, iconPx), tooltip)) {
        m_wantRegionPick = RegionPickerMode::DrawGlobal;
    }
    ImGui::SameLine(0.0f, 2.0f);
    std::snprintf(tooltip, sizeof(tooltip), "Attach to a window (%s) - click the window or draw inside it",
                  m_shortcuts.attachWindow.c_str());
    if (iconButton("##attach-window", iconTextureId(Icon::SquarePen, iconPx), tooltip)) {
        m_wantRegionPick = RegionPickerMode::AttachWindow;
    }
    ImGui::SameLine(0.0f, 2.0f);
    // The face tool sits last among the region tools, before the reset. It
    // is always available where the platform detects faces: whether any
    // face is on screen is the picker overlay's answer to give, not the
    // toolbar's.
    if (supportsFaceDetection()) {
        std::snprintf(tooltip, sizeof(tooltip), "Attach to a face (%s)", m_shortcuts.attachFace.c_str());
        if (iconButton("##attach-face", iconTextureId(Icon::User, iconPx), tooltip)) {
            m_wantRegionPick = RegionPickerMode::AttachFace;
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    const bool fullAlready = isFullScreen();
    if (iconButton("##full-screen", iconTextureId(Icon::Expand, iconPx),
                   fullAlready ? "Reset to full screen (Esc) - already full" : "Reset to full screen (Esc)",
                   fullAlready) &&
        !fullAlready) {
        resetToFullScreen();
    }
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::NewLine();
}

namespace {

void statusRowText(const char* text)
{
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop());
    ImGui::TextUnformatted(text);
}

void drawReadoutChannels(const FloatColor& color, float start, const ReadoutColumns& columns)
{
    const char* labels[3] = {"R", "G", "B"};
    const float channels[3] = {color.r, color.g, color.b};
    for (int channel = 0; channel < 3; ++channel) {
        const float columnStart = start + static_cast<float>(channel) * columns.stride;
        char value[8];
        std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55);
        ImGui::SameLine(columnStart);
        statusRowText(labels[channel]);
        ImGui::SameLine(columnStart + columns.label + columns.gap);
        statusRowText(value);
    }
}

}  // namespace

float App::statusBarHeight()
{
    // The spacing that parts the strip from the panes, the tallest thing that
    // can stand on its row, and the offset that centres the row between them.
    return ImGui::GetStyle().ItemSpacing.y + iconButtonHeight() + statusRowOffset();
}

void App::drawStatusBar()
{
    // The reserved strip under the panes. Output owns its own row - it never
    // paints over the scopes' pixels. Idle, the row spans corner to corner:
    // the pin tool holds the left, the live swatch the right, and the channel
    // readout gathers against the swatch. A message clears the row and takes
    // it whole, so a line that only shows for a moment is not something to be
    // picked out from among the standing furniture.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + statusRowOffset());
    // A full-height anchor opens the row before anything stands on it, so the
    // line's origin never depends on which of them is showing. Without it the
    // first element to be placed sets the origin, and a message - shorter than
    // the tool - dragged everything after it down.
    ImGui::Dummy(ImVec2(0.0f, iconButtonHeight()));
    if (!m_statusMessage.empty() && glfwGetTime() <= m_statusUntil) {
        // Indented to the tool's glyph rather than to the content edge: the
        // row keeps one left edge whichever of the two is standing on it.
        ImGui::SameLine(0.0f, iconButtonInset());
        statusRowText(m_statusMessage.c_str());

        return;
    }
    ImGui::SameLine(0.0f, 0.0f);
    drawPinTool();
    drawCursorReadout(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x);
}

// The tool that samples a colour sits beside the colour it samples, not among
// the region tools - those choose what is captured, this one reads it.
void App::drawPinTool()
{
    const bool pins = pinsAvailable();
    char tooltip[160];
    std::snprintf(tooltip, sizeof(tooltip), "Pin a color (%s)%s", m_shortcuts.pinColor.c_str(),
                  pins ? " - Shift+click a color to pin several" : " - needs a scope that takes pins");
    if (iconButton("##pin-color", iconTextureId(Icon::Pipette, iconPixelSize()), tooltip, !pins) && pins) {
        m_wantRegionPick = RegionPickerMode::PinColor;
    }
}

void App::drawCursorReadout(float taken)
{
    // The colour under the cursor, laid out inwards from its corner: the
    // swatch first, then a named percentage per channel in fixed columns, so
    // no digit coming or going moves anything. The swatch outranks the numbers
    // when the strip runs short, and both give way to whatever already stands
    // on the row.
    if (!m_vectorscopeColor) {
        return;
    }
    const FloatColor& color = *m_vectorscopeColor;
    const float swatch = ImGui::GetTextLineHeight();
    const float swatchStart = ImGui::GetWindowContentRegionMax().x - swatch;
    if (swatchStart < taken + 8.0f) {
        return;
    }
    const ReadoutColumns columns = measureReadoutColumns();
    const float channelsStart = swatchStart - 6.0f - columns.width;
    if (channelsStart >= taken + 8.0f) {
        drawReadoutChannels(color, channelsStart, columns);
    }
    ImGui::SameLine(swatchStart);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop());
    ImGui::ColorButton("##cursor-color", ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(swatch, swatch));
}

void App::setStatus(std::string message)
{
    m_statusMessage = std::move(message);
    m_statusUntil = glfwGetTime() + 2.0;
    m_lastActivity = glfwGetTime();
}

void App::drawScopePanes()
{
    // The enabled scopes stack in activation order along the chosen axis, each
    // pane sized by its weight. A scope's pane point and rect live at its own
    // identity index, so the adaptive block and the context menu read back the
    // right pane.
    m_paneRects.assign(m_scopeRegistry.scopes().size(), ImVec4());
    // One reservation around every pane path, help pages included: whatever
    // fills the area lives in a child sized to leave the status bar's strip
    // below it, so the bar keeps the foot of the window in every state and a
    // centred help block centres on the space it actually has. The host window
    // carries no scrollbar, and neither does this.
    ImGui::BeginChild("##pane-area", ImVec2(0.0f, -statusBarHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawPaneContent();
    ImGui::EndChild();
}

void App::drawPaneContent()
{
    if (!m_captureController.permissionGranted()) {
        drawCaptureHelp("SideScopes cannot see the screen",
                        {
                            "macOS requires the Screen Recording permission.",
                            "",
                            "1. Click the button below",
                            "2. Turn on SideScopes in the list",
                            "3. Quit and reopen SideScopes",
                        },
                        true);

        return;
    }
    if (m_captureController.dead()) {
        const std::string status = m_captureController.status();
        drawCaptureHelp("Screen capture was interrupted", {status, "Reconnecting automatically..."}, false);

        return;
    }
    const std::vector<std::string>& stack = m_view.stack();
    if (stack.size() == 1) {
        drawScopeById(stack.front());
    } else if (stack.size() > 1) {
        drawScopeStack();
    }
}

std::vector<float> App::stackAspects() const
{
    // A module's own declaration wins; the host table covers the color
    // picker and modules that declare nothing.
    std::vector<float> aspects;
    aspects.reserve(m_view.stack().size());
    for (const std::string& scopeId : m_view.stack()) {
        const HostScope* hostScope = m_scopeRegistry.byId(scopeId);
        const float declared =
            hostScope != nullptr && hostScope->descriptor != nullptr ? hostScope->descriptor->preferred_aspect : 0.0f;
        aspects.push_back(declared > 0.0f ? declared : preferredScopeAspect(scopeId));
    }

    return aspects;
}

void App::drawScopeStack()
{
    // Weights split the axis; a divider between each neighboring pair is a thin
    // grab strip that resizes them. Item spacing is zeroed so panes and dividers
    // tile the area exactly, and restored inside each pane so scope contents
    // keep their normal breathing room.
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const int count = static_cast<int>(m_view.stack().size());
    const float divider = DividerThickness * m_uiScale;
    const std::vector<float> weights = m_view.stackWeights();
    const bool sideBySide = resolveSplitDirection(m_view.orientation(), area.x, area.y, weights, stackAspects(),
                                                  divider) == SplitDirection::SideBySide;
    const float axisLength = (sideBySide ? area.x : area.y) - divider * static_cast<float>(count - 1);
    const std::vector<float> lengths = paneLengths(weights, axisLength, MinPaneLength * m_uiScale);
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    for (int pane = 0; pane < count; ++pane) {
        const auto index = static_cast<std::size_t>(pane);
        const ImVec2 paneSize = sideBySide ? ImVec2(lengths[index], area.y) : ImVec2(area.x, lengths[index]);
        ImGui::BeginChild(m_paneIds[index].c_str(), paneSize);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
        drawScopeById(m_view.stack()[index]);
        ImGui::PopStyleVar();
        ImGui::EndChild();
        if (pane + 1 < count) {
            if (sideBySide) {
                ImGui::SameLine();
            }
            drawPaneDivider(pane, sideBySide, divider, area, lengths);
            if (sideBySide) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::PopStyleVar();
}

void App::drawPaneDivider(int leftPane, bool sideBySide, float thickness, const ImVec2& area,
                          const std::vector<float>& lengths)
{
    const ImVec2 size = sideBySide ? ImVec2(thickness, area.y) : ImVec2(area.x, thickness);
    ImGui::InvisibleButton(m_dividerIds[static_cast<std::size_t>(leftPane)].c_str(), size);
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered || active) {
        ImGui::SetMouseCursor(sideBySide ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    paintDivider(sideBySide, hovered || active);
    if (active) {
        adjustDividerWeights(leftPane, sideBySide ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y, lengths);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        equalizeDividerWeights(leftPane);
    }
}

void App::paintDivider(bool sideBySide, bool highlighted)
{
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dividerDraw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(highlighted ? ImGuiCol_SeparatorActive : ImGuiCol_Separator);
    if (sideBySide) {
        const float mid = (min.x + max.x) * 0.5f;
        dividerDraw->AddLine(ImVec2(mid, min.y + 2.0f), ImVec2(mid, max.y - 2.0f), color, 1.0f);
    } else {
        const float mid = (min.y + max.y) * 0.5f;
        dividerDraw->AddLine(ImVec2(min.x + 2.0f, mid), ImVec2(max.x - 2.0f, mid), color, 1.0f);
    }
}

void App::adjustDividerWeights(int leftPane, float deltaPixels, const std::vector<float>& lengths)
{
    if (deltaPixels == 0.0f) {
        return;
    }
    const auto left = static_cast<std::size_t>(leftPane);
    const std::string& leftId = m_view.stack()[left];
    const std::string& rightId = m_view.stack()[left + 1];
    const auto [newLeft, newRight] = dragDividerWeights(m_view.weight(leftId), m_view.weight(rightId), lengths[left],
                                                        lengths[left + 1], deltaPixels, MinPaneLength * m_uiScale);
    m_view.setWeight(leftId, newLeft);
    m_view.setWeight(rightId, newRight);
}

void App::equalizeDividerWeights(int leftPane)
{
    // Double-click reset: the two neighbors share their combined weight evenly,
    // leaving every other pane untouched.
    const auto left = static_cast<std::size_t>(leftPane);
    const std::string& leftId = m_view.stack()[left];
    const std::string& rightId = m_view.stack()[left + 1];
    const float average = (m_view.weight(leftId) + m_view.weight(rightId)) * 0.5f;
    m_view.setWeight(leftId, average);
    m_view.setWeight(rightId, average);
    m_nextPreferencesSave = glfwGetTime() + 1.0;
    m_lastActivity = glfwGetTime();
}

void App::drawScopeById(std::string_view id)
{
    const auto index = static_cast<std::size_t>(m_scopeRegistry.indexOf(id));
    m_panePoints[index] = ImGui::GetContentRegionAvail();
    const ImVec2 paneMin = ImGui::GetCursorScreenPos();
    const ImVec2 paneAvail = ImGui::GetContentRegionAvail();
    m_paneRects[index] = ImVec4(paneMin.x, paneMin.y, paneMin.x + paneAvail.x, paneMin.y + paneAvail.y);
    if (id == VectorscopeScopeId) {
        drawVectorscopePane();
    } else if (id == HistogramScopeId) {
        const ScopeInstance* instance = projectionFor(HistogramScopeId);
        if (instance != nullptr) {
            drawHistogram(textureForId(HistogramScopeId), m_output, *instance, currentHistogramStyle(),
                          m_view.graticule(), m_vectorscopeColor, m_histogramScratch);
        }
    } else if (id == ColorPickerScopeId) {
        drawColorPicker(m_vectorscopeColor, m_pins, m_callbackState.monospaceFont);
    } else {
        drawWaveformPane(id);
    }
}

void App::drawVectorscopePane()
{
    const DrawnScope scope = drawScopeImage(textureForId(VectorscopeScopeId), true, static_cast<float>(m_view.zoom()));
    const SsParamInfo* gain = firstParamOfKind(descriptorFor(VectorscopeScopeId), SS_PARAM_INTENSITY);
    if (gain != nullptr) {
        if (const auto adjusted = traceIntensityGesture(scope, VectorscopeScopeId, m_view.intensity(VectorscopeScopeId),
                                                        static_cast<float>(gain->default_value),
                                                        static_cast<float>(gain->intensity_shift), m_flash)) {
            m_view.setIntensity(VectorscopeScopeId, adjusted->intensity);
            m_analysis.scopeParams[VectorscopeScopeId][gain->key] = adjusted->gain;
            m_analysisDirty = true;
        }
    }
    // Zoomed overlays run past the pane; clip them to it.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(scope.origin, ImVec2(scope.origin.x + scope.size.x, scope.origin.y + scope.size.y), true);
    const ScopeInstance* instance = projectionFor(VectorscopeScopeId);
    if (instance != nullptr) {
        if (m_view.graticule()) {
            drawGraticule(scope, instance->graticule(), GraticuleStyle{VectorscopeMajorLineWidth});
        }
        // Pinned references are host state, drawn amber over the trace; the live
        // cursor point takes its own white default.
        for (const FloatColor& pinned : m_pins.colors()) {
            drawMarkers(scope, instance->markers(toSsColor(pinned)), PinnedPointColor);
        }
        if (m_vectorscopeColor) {
            drawMarkers(scope, instance->markers(toSsColor(*m_vectorscopeColor)));
        }
    }
    draw->PopClipRect();
    if (m_view.zoom() > 1) {
        char badge[4] = {static_cast<char>('0' + m_view.zoom()), 'x', '\0'};
        draw->AddText(ImVec2(scope.origin.x + scope.size.x - 26, scope.origin.y + 6), GraticuleLabel, badge);
    }
}

void App::drawWaveformPane(std::string_view id)
{
    // The waveform and its parade share one intensity control; each draws its
    // own instance's scale and cursor markers, and the module's marker layout
    // already follows its configured mode, so the host needs no branch.
    const DrawnScope scope = drawScopeImage(textureForId(id), false);
    const SsParamInfo* gain = firstParamOfKind(descriptorFor(WaveformScopeId), SS_PARAM_INTENSITY);
    if (gain != nullptr) {
        if (const auto adjusted = traceIntensityGesture(scope, WaveformScopeId, m_view.intensity(WaveformScopeId),
                                                        static_cast<float>(gain->default_value),
                                                        static_cast<float>(gain->intensity_shift), m_flash)) {
            m_view.setIntensity(WaveformScopeId, adjusted->intensity);
            setWaveformGain(adjusted->gain);
            m_analysisDirty = true;
        }
    }
    const ScopeInstance* instance = projectionFor(id);
    if (instance != nullptr) {
        if (m_view.graticule()) {
            drawGraticule(scope, instance->graticule(), GraticuleStyle{});
        }
        if (m_waveformColor) {
            drawMarkers(scope, instance->markers(toSsColor(*m_waveformColor)));
        }
    }
}

void App::drawAboutWindow()
{
    if (!m_showAbout) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("About SideScopes", &m_showAbout, ImGuiWindowFlags_NoCollapse);
    // The window title carries the name; the body leads with the version.
    ImGui::Text("Version %s", m_versionInfo.display.c_str());
    if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(m_versionInfo.display.c_str());
    }
    wrappedTooltip("click to copy");
    ImGui::Separator();
    // Clickable link text in the accent color, underlined, opening the
    // destination in the default browser; the tooltip names the URL.
    const auto link = [](const char* text, const char* url) {
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(lo.x, hi.y), ImVec2(hi.x, hi.y), ImGui::GetColorU32(accent));
        if (ImGui::IsItemClicked()) {
            openUrl(url);
        }
        wrappedTooltip(url);
    };
    link("sidescopes.org", "https://sidescopes.org");
    link("github.com/sidescopes/sidescopes", "https://github.com/sidescopes/sidescopes");
    ImGui::Separator();
    ImGui::TextDisabled("GPL-3.0-or-later");
    ImGui::End();
}

void App::handleContextMenu()
{
    // Right-click: the native menu carries the modes and toggles.
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Right) ||
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    // Which pane the click landed in decides which options lead the menu.
    const ImVec2 mouse = ImGui::GetMousePos();
    int clickedPane = -1;
    for (std::size_t pane = 0; pane < m_paneRects.size(); ++pane) {
        const ImVec4& rect = m_paneRects[pane];
        if (rect.z <= rect.x || rect.w <= rect.y) {
            continue;
        }
        if (mouse.x >= rect.x && mouse.x < rect.z && mouse.y >= rect.y && mouse.y < rect.w) {
            clickedPane = static_cast<int>(pane);
        }
    }
    std::vector<NativeMenuItem> menu;
    std::vector<ParamMenuAction> paramActions;
    const ContextMenuModel model{
        m_view,          m_scopeRegistry, m_shortcuts,        m_scopeShortcuts,    m_analysis.scopeParams, m_attach,
        m_layoutPresets, m_pins.empty(),  m_activePresetSlot, m_userUiScaleFactor, isFullScreen()};
    buildContextMenu(model, clickedPane, menu, paramActions);
    const int chosen = showNativeContextMenu(menu);
    dispatchMenuChoice(chosen, paramActions);
}

const std::map<std::string, double>& App::paramsOf(std::string_view id) const
{
    static const std::map<std::string, double> noParams;
    const auto stored = m_analysis.scopeParams.find(std::string{id});

    return stored != m_analysis.scopeParams.end() ? stored->second : noParams;
}

void App::dispatchMenuChoice(int chosen, const std::vector<ParamMenuAction>& paramActions)
{
    // A dynamic id sets one scope parameter from the side table; the fixed ids
    // drive the host actions.
    if (chosen >= ParamMenuActionBase) {
        const std::size_t index = static_cast<std::size_t>(chosen - ParamMenuActionBase);
        if (index < paramActions.size()) {
            const ParamMenuAction& picked = paramActions[index];
            m_analysis.scopeParams[picked.scopeId][picked.paramKey] = picked.value;
            m_analysisDirty = true;
        }
    }
    dispatchScopeToggleMenu(chosen);
    dispatchRegionMenu(chosen);
    dispatchViewMenu(chosen);
    dispatchLayoutMenu(chosen);
    dispatchUiScaleMenu(chosen);
    m_lastActivity = glfwGetTime();
    m_nextPreferencesSave = glfwGetTime() + 1.0;
}

void App::dispatchScopeToggleMenu(int chosen)
{
    switch (chosen) {
    case MenuShowVectorscope:
        toggleScope(VectorscopeScopeId);
        break;
    case MenuShowWaveform:
        toggleScope(WaveformScopeId);
        break;
    case MenuShowWaveformParade:
        toggleScope(ParadeScopeId);
        break;
    case MenuShowHistogram:
        toggleScope(HistogramScopeId);
        break;
    case MenuShowColorPicker:
        toggleScope(ColorPickerScopeId);
        break;
    default:
        break;
    }
}

void App::dispatchRegionMenu(int chosen)
{
    switch (chosen) {
    case MenuAttachWindow:
        m_wantRegionPick = RegionPickerMode::AttachWindow;
        break;
    case MenuDrawRegion:
        m_wantRegionPick = RegionPickerMode::DrawGlobal;
        break;
    case MenuAttachFace:
        m_wantRegionPick = RegionPickerMode::AttachFace;
        break;
    case MenuFullScreen:
    case MenuDetachAll:
        resetToFullScreen();
        break;
    case MenuDetachWindow:
        detachActiveWindow();
        break;
    case MenuPinColor:
        m_wantRegionPick = RegionPickerMode::PinColor;
        break;
    case MenuClearPinnedMarkers:
        m_pins.clear();
        break;
    default:
        break;
    }
}

// Opens the folder holding the diagnostic log, so "send the log" is a
// click instead of a hunt through the temp directory.
void openDiagLogFolder()
{
    std::string folder = diagLogPath();
    std::replace(folder.begin(), folder.end(), '\\', '/');
    const std::size_t cut = folder.find_last_of('/');
    if (cut == std::string::npos) {
        return;  // a bare file name names no folder to show
    }
    folder.resize(cut == 0 ? 1 : cut);  // a file at the root keeps the root
    const std::string url = (folder.front() == '/' ? "file://" : "file:///") + folder;
    openUrl(url.c_str());
}

void App::dispatchViewMenu(int chosen)
{
    switch (chosen) {
    case MenuZoom1:
        m_view.setZoom(1);
        break;
    case MenuZoom2:
        m_view.setZoom(2);
        break;
    case MenuZoom4:
        m_view.setZoom(4);
        break;
    case MenuToggleGraticule:
        m_view.setGraticule(!m_view.graticule());
        break;
    case MenuToggleCaptureVisibility:
        setCaptureVisibility(!captureVisible());
        break;
    case MenuToggleDiagRecording:
        // The menu records everything; channel selection stays with the
        // SIDESCOPES_DIAG environment for development use.
        diagConfigure(diagRecording() ? DiagConfig{} : DiagConfig{"all", "", DiagFlush::Interval});
        break;
    case MenuShowDiagLog:
        openDiagLogFolder();
        break;
    case MenuResetDiagnostics:
        setCaptureVisibility(false);
        if (diagRecording()) {
            diagConfigure(DiagConfig{});
        }
        break;
    case MenuAbout:
        m_showAbout = true;
        break;
    case MenuOpenSettings:
        m_showSettings = true;
        break;
    case MenuQuit:
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        break;
    default:
        break;
    }
}

void App::dispatchLayoutMenu(int chosen)
{
    // Orientation is a direct set; the preset ranges each map their id back to a
    // slot. Persistence rides dispatchMenuChoice's tail.
    switch (chosen) {
    case MenuLayoutAuto:
        m_view.setOrientation(LayoutOrientation::Automatic);

        return;
    case MenuLayoutVertical:
        m_view.setOrientation(LayoutOrientation::Vertical);

        return;
    case MenuLayoutHorizontal:
        m_view.setOrientation(LayoutOrientation::Horizontal);

        return;
    default:
        break;
    }
    if (chosen > MenuLoadPresetBase && chosen <= MenuLoadPresetBase + LayoutPresetSlots) {
        loadLayoutPreset(chosen - MenuLoadPresetBase);
    } else if (chosen > MenuSavePresetBase && chosen <= MenuSavePresetBase + LayoutPresetSlots) {
        saveLayoutPreset(chosen - MenuSavePresetBase);
    }
}

void App::dispatchUiScaleMenu(int chosen)
{
    const int step = chosen - MenuUiScaleBase;
    if (step < 0 || step >= static_cast<int>(UiScaleSteps.size())) {
        return;
    }
    m_userUiScaleFactor = UiScaleSteps[static_cast<std::size_t>(step)];
    refreshUiScale();
}

void App::commitAnalysisChanges()
{
    if (m_analysisDirty) {
        m_worker.updateSettings(m_analysis);
        configureProjectionInstances();
        syncRegionBorder();
        m_analysisDirty = false;
        m_lastActivity = glfwGetTime();
        m_nextPreferencesSave = glfwGetTime() + 1.0;
    }
    if (m_nextPreferencesSave > 0.0 && glfwGetTime() > m_nextPreferencesSave) {
        persistPreferences();
        m_nextPreferencesSave = -1.0;
    }
}

}  // namespace sidescopes
