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

#include "app/capture_controller.h"
#include "app/overlay_render.h"
#include "app/param_menu.h"
#include "app/pin_board.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/version.h"
#include "app/window_suggestions.h"
#include "core/analysis_worker.h"
#include "core/color_lab.h"
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

// Fixed ids for the host actions. Scope parameter choices are dynamic: they
// carry ids from ParamMenuActionBase upward, resolved through a per-open side
// table, never through this enum.
enum MenuAction
{
    MenuShowVectorscope = 1,
    MenuShowWaveform,
    MenuShowWaveformParade,
    MenuShowHistogram,
    MenuShowColorPicker,
    MenuDrawRegion = 25,
    MenuPickFaces,
    MenuZoom1,
    MenuZoom2,
    MenuZoom4,
    MenuSelectRegion = 30,
    MenuFullScreenRegion,
    MenuDetachWindow,
    MenuDetachAll,
    MenuToggleGraticule = 40,
    MenuClearPinnedMarkers,
    MenuPickPinColor,
    MenuOpenSettings = 50,
    MenuAbout,
    MenuQuit,
};

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
        size = ImVec2(texture.width() * scale, texture.height() * scale);
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
    ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

// Region tool icons mirror the cursors of their modes: a pointing hand
// for picking a window (the pick-mode hover cursor), a crosshair
// for drawing one (the draw-mode cursor), an eyedropper for pinning
// colors, expanding arrows for full screen.
enum class RegionIcon
{
    PickHand,
    Crosshair,
    Face,
    Dropper,
    Expand
};

// A simplified pointing hand, the shape of the pick-mode cursor. Traced from
// the classic cursor-hand outline: tall index left of center, three knuckle
// stubs descending to the right, the thumb web sweeping down-left, a flat cuff.
void drawPickHandIcon(ImDrawList* draw, const ImVec2& center, ImU32 color)
{
    const ImVec2 outline[] = {
        {-2.8f, 7.5f},  {-5.6f, 1.8f},  {-6.0f, 0.4f},  {-5.6f, -0.4f}, {-4.6f, -0.6f}, {-2.7f, 0.6f},
        {-2.7f, -6.6f}, {-2.2f, -7.5f}, {-1.1f, -7.5f}, {-0.6f, -6.6f}, {-0.6f, -2.8f}, {0.1f, -3.3f},
        {1.1f, -3.3f},  {1.5f, -2.6f},  {1.7f, -2.4f},  {2.4f, -2.5f},  {2.9f, -1.8f},  {3.2f, -1.6f},
        {4.0f, -1.5f},  {4.5f, -0.8f},  {4.5f, 6.3f},   {3.9f, 7.5f},
    };
    ImVec2 points[std::size(outline)];
    for (std::size_t i = 0; i < std::size(outline); ++i) {
        points[i] = ImVec2(center.x + outline[i].x, center.y + outline[i].y);
    }
    draw->AddPolyline(points, static_cast<int>(std::size(outline)), color, ImDrawFlags_Closed, 1.4f);
}

// The draw-mode crosshair: long thin beams, small center gap.
void drawCrosshairIcon(ImDrawList* draw, const ImVec2& center, ImU32 color)
{
    const auto beam = [&](float dx, float dy) {
        draw->AddLine(ImVec2(center.x + dx * 1.25f, center.y + dy * 1.25f),
                      ImVec2(center.x + dx * 7.5f, center.y + dy * 7.5f), color, 1.4f);
    };
    beam(0.0f, -1.0f);
    beam(0.0f, 1.0f);
    beam(-1.0f, 0.0f);
    beam(1.0f, 0.0f);
}

// A face: head outline, two eyes, a smile arc.
void drawFaceIcon(ImDrawList* draw, const ImVec2& center, ImU32 color)
{
    draw->AddCircle(center, 7.5f, color, 0, 1.4f);
    draw->AddCircleFilled(ImVec2(center.x - 2.8f, center.y - 2.0f), 1.1f, color);
    draw->AddCircleFilled(ImVec2(center.x + 2.8f, center.y - 2.0f), 1.1f, color);
    ImVec2 smile[5];
    for (int i = 0; i < 5; ++i) {
        const float angle = (0.30f + 0.10f * i) * 3.14159265f;
        smile[i] = ImVec2(center.x + 3.3f * std::cos(angle), center.y + 3.3f * std::sin(angle));
    }
    draw->AddPolyline(smile, 5, color, ImDrawFlags_None, 1.4f);
}

// The classic pipette silhouette, filled so it reads at chip size: round bulb, a
// wider collar band, a long tapering tip, and a drop fallen just past it.
void drawDropperIcon(ImDrawList* draw, const ImVec2& center, ImU32 color)
{
    draw->AddCircleFilled(ImVec2(center.x + 4.4f, center.y - 4.4f), 2.4f, color);
    draw->AddLine(ImVec2(center.x + 1.7f, center.y - 1.7f), ImVec2(center.x + 3.0f, center.y - 3.0f), color, 4.2f);
    draw->AddTriangleFilled(ImVec2(center.x - 5.3f, center.y + 5.3f), ImVec2(center.x + 1.3f, center.y + 0.7f),
                            ImVec2(center.x - 0.7f, center.y - 1.3f), color);
    draw->AddCircleFilled(ImVec2(center.x - 6.6f, center.y + 6.6f), 1.0f, color);
}

// Two arrows expanding to opposite corners, the fullscreen idiom.
void drawExpandIcon(ImDrawList* draw, const ImVec2& center, const ImVec2& a, const ImVec2& b, ImU32 color, float stroke)
{
    const auto arrow = [&](ImVec2 from, ImVec2 to, float headX, float headY) {
        draw->AddLine(from, to, color, stroke);
        draw->AddLine(to, ImVec2(to.x + headX * 3.5f, to.y), color, stroke);
        draw->AddLine(to, ImVec2(to.x, to.y + headY * 3.5f), color, stroke);
    };
    arrow(ImVec2(center.x - 1.5f, center.y + 1.5f), ImVec2(a.x + 0.5f, b.y - 0.5f), 1, -1);
    arrow(ImVec2(center.x + 1.5f, center.y - 1.5f), ImVec2(b.x - 0.5f, a.y + 0.5f), -1, 1);
}

bool iconButton(const char* id, RegionIcon icon, const char* tooltip, bool dimmed = false)
{
    const float height = ImGui::GetTextLineHeight() + 4.0f;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(height + 8.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    const ImVec2 center(std::floor((min.x + max.x) / 2) + 0.5f, std::floor((min.y + max.y) / 2) + 0.5f);
    const float half = 7.0f;
    const ImVec2 a(center.x - half, center.y - half + 1.0f);
    const ImVec2 b(center.x + half, center.y + half - 1.0f);
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text, dimmed ? 0.4f : 1.0f);
    const float stroke = 1.5f;
    if (icon == RegionIcon::PickHand) {
        drawPickHandIcon(draw, center, color);
    } else if (icon == RegionIcon::Crosshair) {
        drawCrosshairIcon(draw, center, color);
    } else if (icon == RegionIcon::Face) {
        drawFaceIcon(draw, center, color);
    } else if (icon == RegionIcon::Dropper) {
        drawDropperIcon(draw, center, color);
    } else {
        drawExpandIcon(draw, center, a, b, color, stroke);
    }
    ImGui::SetItemTooltip("%s", tooltip);

    return pressed;
}

void refreshFacePresence(AnalysisWorker& worker, uint32_t displayId, AppCallbackState& state)
{
    if (!supportsFaceDetection()) {
        return;
    }
    if (state.faceCheckRunning.exchange(true)) {
        return;
    }
    // Detection takes long enough to hitch a frame, so it runs on a copy
    // of the latest frame in a background thread.
    auto pixels = std::make_shared<std::vector<uint8_t>>();
    int width = 0;
    int height = 0;
    const bool captured = worker.withLatestFrame([&](const FrameView& view) {
        width = view.width;
        height = view.height;
        pixels->resize(static_cast<std::size_t>(view.height) * view.strideBytes);
        std::memcpy(pixels->data(), view.bgra, pixels->size());
    });
    if (!captured || width == 0 || height == 0) {
        state.faceCheckRunning.store(false);
        return;
    }
    float pixelsPerPoint = 1.0f;
    if (const auto geometry = geometryOfDisplay(displayId)) {
        pixelsPerPoint = static_cast<float>(width / geometry->widthPoints);
    }
    // The detached check captures a pointer to the state, valid because
    // main() owns it and only starts checks from inside the frame loop;
    // faceCheckRunning serializes to one in flight, and main drains that
    // one after the loop before the state leaves scope.
    AppCallbackState* statePtr = &state;
    std::thread([pixels, width, height, pixelsPerPoint, statePtr] {
        const FrameView view{pixels->data(), width * 4, width, height, ColorSpaceHint::Srgb, 0};
        statePtr->facesOnScreen.store(static_cast<int>(detectFaces(view, pixelsPerPoint).size()));
        statePtr->faceCheckRunning.store(false);
    }).detach();
}

// The secure-CRT deprecations make std::getenv and std::fopen hard errors
// under MSVC's warnings-as-errors, so the debug-dump plumbing goes through
// the annexes Microsoft accepts.
bool debugSuggestionsRequested()
{
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "SIDESCOPES_DEBUG_SUGGESTIONS") != 0 || value == nullptr) {
        return false;
    }
    std::free(value);
    return true;
#else
    return std::getenv("SIDESCOPES_DEBUG_SUGGESTIONS") != nullptr;
#endif
}

std::FILE* openDebugFile(const char* path, const char* mode)
{
#ifdef _MSC_VER
    std::FILE* file = nullptr;
    return fopen_s(&file, path, mode) == 0 ? file : nullptr;
#else
    return std::fopen(path, mode);
#endif
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
    float scaleX = 2.0f;
    float scaleY = 2.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    ImFontConfig config;
    config.RasterizerDensity = scaleX;
    ImGuiIO& io = ImGui::GetIO();
    bool loaded = false;
    for (const std::string& path : interfaceFontFiles()) {
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 13.0f, &config)) {
            loaded = true;
            break;
        }
    }
    ImFont* monospace = nullptr;
    const float monoSize = 13.0f * monospaceFontScale();
    for (const std::string& path : monospaceFontFiles()) {
        if ((monospace = io.Fonts->AddFontFromFileTTF(path.c_str(), monoSize, &config))) {
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
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    const float density = windowWidth > 0 ? framebufferWidth / static_cast<float>(windowWidth) : 1.0f;
    return density > 0 ? scaleX / density : scaleX;
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

// The on-screen windows of a display become its region suggestions. The
// platform query lives here; buildWindowSuggestions owns the choice of which
// windows to offer and in what order.
std::vector<SuggestedRegion> windowSuggestionsFor(uint32_t displayId)
{
    const auto geometry = geometryOfDisplay(displayId);
    if (!geometry) {
        return {};
    }

    // The most windows the picker offers at once, so the overlay stays
    // readable on a crowded desktop.
    constexpr int MaxWindowSuggestions = 5;

    return buildWindowSuggestions(onScreenWindows(displayId), *geometry, MaxWindowSuggestions);
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
        const float* plane = output.histogramOutline.data() + channel * Histogram::Bins;
        const float bandTop = scope.origin.y + (bands ? channel * scope.size.y / 3.0f : 0.0f);
        const float bandHeight = bands ? scope.size.y / 3.0f : scope.size.y;
        points.clear();
        for (int sample = 0; sample < samples; ++sample) {
            const float binPosition =
                std::clamp((sample + 0.5f) * Histogram::Bins / samples - 0.5f, 0.0f, Histogram::Bins - 1.0f);
            const int center = static_cast<int>(binPosition);
            const float t = binPosition - center;
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
            points.push_back(ImVec2(scope.origin.x + (sample + 0.5f) * scope.size.x / samples, y));
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

// The color picker pane: the sampled cursor color as a large
// swatch with its values spelled out three ways at once - 0-255,
// percent, and hex - because matching a reference means never
// converting in your head. Clicking the swatch or the hex line
// copies the hex; the session's pinned colors (P) ride along as
// small swatches with the same click.
// Managing a pin happens where the pin lives; the app-wide
// native menu yields to this popup.
// Hex codes render in the fixed-width font when one loaded, so
// every code is the same width and columns anchor exactly.
void drawPinnedMenu(PinBoard& pins)
{
    if (!ImGui::BeginPopup("##pinned-menu")) {
        return;
    }
    if (pins.managed() >= 0 && pins.managed() < static_cast<int>(pins.size())) {
        const std::size_t chosen = static_cast<std::size_t>(pins.managed());
        char chosenHex[8];
        std::snprintf(
            chosenHex, sizeof(chosenHex), "#%02X%02X%02X", static_cast<int>(std::lround(pins.color(chosen).r)),
            static_cast<int>(std::lround(pins.color(chosen).g)), static_cast<int>(std::lround(pins.color(chosen).b)));
        if (ImGui::MenuItem(chosenHex)) {
            ImGui::SetClipboardText(chosenHex);
        }
        if (ImGui::MenuItem("Remove")) {
            pins.removeAt(chosen);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear All")) {
            pins.clear();
        }
    }
    ImGui::EndPopup();
}

// The hex and signed values align in a fixed-width font when the system had
// one; the font is pushed rather than sized by hand so global UI scale applies
// once. A null font falls back to the interface font.
void pushHexFont(ImFont* font)
{
    if (font) {
        ImGui::PushFont(font);
    }
}

void popHexFont(ImFont* font)
{
    if (font) {
        ImGui::PopFont();
    }
}

float hexFontWidth(ImFont* font, const char* text)
{
    pushHexFont(font);
    const float measured = ImGui::CalcTextSize(text).x;
    popHexFont(font);

    return measured;
}

void hexFontText(ImFont* font, const char* text)
{
    pushHexFont(font);
    ImGui::TextUnformatted(text);
    popHexFont(font);
}

void hexFontTextDisabled(ImFont* font, const char* text)
{
    pushHexFont(font);
    ImGui::TextDisabled("%s", text);
    popHexFont(font);
}

ImVec4 pickerSwatchColor(const FloatColor& source)
{
    return ImVec4(source.r / 255.0f, source.g / 255.0f, source.b / 255.0f, 1.0f);
}

void pinHexOf(const PinBoard& pins, std::size_t index, char* buffer)
{
    std::snprintf(buffer, 8, "#%02X%02X%02X", static_cast<int>(std::lround(pins.color(index).r)),
                  static_cast<int>(std::lround(pins.color(index).g)),
                  static_cast<int>(std::lround(pins.color(index).b)));
}

// Ink and its legibility shadow both follow the color beneath: dark ink over a
// light shadow on light colors, light ink over a dark shadow on dark ones.
ImU32 pickerLabelInk(const FloatColor& under)
{
    const float luma = (54.0f * under.r + 183.0f * under.g + 19.0f * under.b) / 256.0f;

    return luma > 140.0f ? IM_COL32(0, 0, 0, 170) : IM_COL32(255, 255, 255, 180);
}

ImU32 pickerLabelShadow(const FloatColor& under)
{
    const float luma = (54.0f * under.r + 183.0f * under.g + 19.0f * under.b) / 256.0f;

    return luma > 140.0f ? IM_COL32(255, 255, 255, 64) : IM_COL32(0, 0, 0, 115);
}

// One string over its shadow, in an optional font: the hex rows pass the
// fixed-width font, everything else draws in the interface font.
void swatchText(ImDrawList* draw, const ImVec2& pos, ImU32 ink, ImU32 shadow, const char* text, ImFont* font = nullptr)
{
    if (font) {
        ImGui::PushFont(font, font->LegacySize);
    }
    draw->AddText(ImVec2(pos.x, pos.y + 1.0f), shadow, text);
    draw->AddText(pos, ink, text);
    if (font) {
        ImGui::PopFont();
    }
}

// Tooltip strings shared by the deck header and its rows.
constexpr const char* PickerMatchTip =
    "similarity to the live color: 100% is identical, 0% is as far apart as black and white (CIEDE2000) - sRGB assumed";
constexpr const char* PickerLchTip =
    "lightness, chroma, and hue difference, live minus pinned - hue weighted by chroma, sRGB assumed";
constexpr const char* PickerRgbTip = "channel difference, live minus pinned";

// The live color, its formatted values, and the shared column metrics every
// picker section measures against. pins is borrowed for the length of one draw.
struct PickerContext
{
    FloatColor color;
    PinBoard& pins;
    ImFont* monospaceFont;
    float labelColumn;
    float percentColumn;
    float columnGap;
    float channelStride;
    float hexWidth;
    float lineHeight;
    const char* hex;
};

// The comparator geometry: the size tier, the split against a pinned reference,
// and the on-swatch/solo readout placements the tier admits.
struct PickerHero
{
    bool tiny;
    bool full;
    bool split;
    bool onSwatch;
    bool soloOnSwatch;
    float heroHeight;
    float heroWidth;
    float valuesStart;
    ImVec2 heroOrigin;
    float pad;
    float rowHeight;
    float seamX;
    float blockWidth;
    float blockTop;
};

// The deck's admitted numeric groups and the right edge of every visible
// column, walked once so the header and every row share them.
struct DeckLayout
{
    float swatchX;
    float hexX;
    bool showMatch;
    bool showLch;
    bool showRgb;
    float matchRight;
    float lchRight[3];
    float rgbRight[3];
    float matchTypical;
    float lchTypical;
    float rgbTypical;
};

void drawPickerNoColor(const ImVec2& area, float lineHeight)
{
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (area.y - lineHeight) / 2.0f)));
    const char* hint = "no color under the cursor yet";
    const float width = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
    ImGui::TextDisabled("%s", hint);
}

// The split never depends on the tier - only its height does. Reads the cursor,
// so it runs at the hero's start, before anything draws.
PickerHero computePickerHero(const PickerContext& ctx, const ImVec2& area, const ImGuiStyle& style)
{
    PickerHero hero{};
    hero.tiny = area.y < 120.0f;
    hero.full = area.y >= 240.0f;
    // With nothing pinned there is no deck to reserve for, and an uncapped
    // comparator absorbs the pane instead of leaving a dead black field.
    const float deckReserve = ctx.pins.empty() ? 0.0f : (hero.full ? 4.0f * ctx.lineHeight : ctx.lineHeight);
    const float reserved = 2.0f * ctx.lineHeight + deckReserve + style.ItemSpacing.y;
    const float heroCap = ctx.pins.empty() ? area.y : (hero.full ? 220.0f : 64.0f);
    hero.heroHeight = hero.tiny ? ctx.lineHeight * 1.5f : std::clamp(area.y - reserved, 48.0f, heroCap);
    hero.split = ctx.pins.hasComparator();
    hero.heroWidth = area.x;
    hero.heroOrigin = ImGui::GetCursorScreenPos();
    hero.valuesStart = ImGui::GetCursorPosX();
    hero.pad = 8.0f;
    hero.rowHeight = ctx.lineHeight;
    hero.seamX = hero.heroOrigin.x + hero.heroWidth / 2.0f;
    hero.blockWidth = ctx.labelColumn + ctx.columnGap + ctx.percentColumn;
    const float blockBottom = hero.heroOrigin.y + hero.heroHeight - hero.pad;
    hero.blockTop = blockBottom - 4.0f * hero.rowHeight;
    // A split half carries the readout only when it is tall enough for four rows
    // and wide enough to hold the block clear of its corner label; hex can be
    // the widest row, so it drives the width test.
    const float blockExtent = std::max(hero.blockWidth, ctx.hexWidth);
    hero.onSwatch = hero.split && hero.heroHeight >= 6.0f * ctx.lineHeight &&
                    hero.heroWidth / 2.0f >= blockExtent + 2.0f * hero.pad + ImGui::CalcTextSize("LIVE").x;
    hero.soloOnSwatch = !hero.split && hero.heroHeight >= 3.5f * ctx.lineHeight &&
                        3.0f * ctx.channelStride + ctx.hexWidth + 2.0f * hero.pad <= hero.heroWidth;

    return hero;
}

// One hero half's readout: labels in a fixed column, percentages right-aligned
// to the seam, hex on the fourth row. The left half mirrors the right.
void drawPickerSwatchBlock(ImDrawList* draw, const PickerContext& ctx, const PickerHero& hero, const FloatColor& swatch,
                           bool leftHalf)
{
    const ImU32 ink = pickerLabelInk(swatch);
    const ImU32 shadow = pickerLabelShadow(swatch);
    const float channels[3] = {swatch.r, swatch.g, swatch.b};
    const char* labels[3] = {"R", "G", "B"};
    for (int channel = 0; channel < 3; ++channel) {
        const float y = hero.blockTop + static_cast<float>(channel) * hero.rowHeight;
        char value[8];
        std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55f);
        const float valueWidth = ImGui::CalcTextSize(value).x;
        const float labelX = leftHalf ? hero.seamX - hero.pad - hero.blockWidth : hero.seamX + hero.pad;
        const float valueX =
            leftHalf ? hero.seamX - hero.pad - valueWidth : hero.seamX + hero.pad + hero.blockWidth - valueWidth;
        swatchText(draw, ImVec2(labelX, y), ink, shadow, labels[channel]);
        swatchText(draw, ImVec2(valueX, y), ink, shadow, value);
    }
    char blockHex[8];
    std::snprintf(blockHex, sizeof(blockHex), "#%02X%02X%02X",
                  static_cast<int>(std::lround(std::clamp(swatch.r, 0.0f, 255.0f))),
                  static_cast<int>(std::lround(std::clamp(swatch.g, 0.0f, 255.0f))),
                  static_cast<int>(std::lround(std::clamp(swatch.b, 0.0f, 255.0f))));
    const float hexY = hero.blockTop + 3.0f * hero.rowHeight;
    const float hexX = leftHalf ? hero.seamX - hero.pad - ctx.hexWidth : hero.seamX + hero.pad;
    swatchText(draw, ImVec2(hexX, hexY), ink, shadow, blockHex, ctx.monospaceFont);
}

// Solo, one line along the swatch foot carries the same reading when the hero is
// tall enough and the whole line fits across it.
void drawPickerSoloReadout(ImDrawList* draw, const PickerContext& ctx, const PickerHero& hero)
{
    const ImU32 ink = pickerLabelInk(ctx.color);
    const ImU32 shadow = pickerLabelShadow(ctx.color);
    draw->AddText(ImVec2(hero.heroOrigin.x + 5, hero.heroOrigin.y + 3), ink, "LIVE");
    const float baselineY = hero.heroOrigin.y + hero.heroHeight - hero.pad - hero.rowHeight;
    const float startX = hero.heroOrigin.x + hero.pad;
    const float channels[3] = {ctx.color.r, ctx.color.g, ctx.color.b};
    const char* labels[3] = {"R", "G", "B"};
    for (int channel = 0; channel < 3; ++channel) {
        const float columnStart = startX + static_cast<float>(channel) * ctx.channelStride;
        char value[8];
        std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55f);
        swatchText(draw, ImVec2(columnStart, baselineY), ink, shadow, labels[channel]);
        const float valueX =
            columnStart + ctx.labelColumn + ctx.columnGap + ctx.percentColumn - ImGui::CalcTextSize(value).x;
        swatchText(draw, ImVec2(valueX, baselineY), ink, shadow, value);
    }
    swatchText(draw, ImVec2(startX + 3.0f * ctx.channelStride, baselineY), ink, shadow, ctx.hex, ctx.monospaceFont);
}

// The comparator: the live color, split against the selected pin when one is
// loaded. Touching halves make small casts visible where separated swatches
// hide them. Draws into the host window, so it fetches that draw list here.
void drawPickerHero(const PickerContext& ctx, const PickerHero& hero)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // The live swatch renders but never copies: the sample follows the cursor,
    // so a click would destroy the very color it shows.
    ImGui::ColorButton("##picker-live", pickerSwatchColor(ctx.color),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(hero.split ? hero.heroWidth / 2.0f : hero.heroWidth, hero.heroHeight));
    if (hero.split) {
        char pinHex[8];
        pinHexOf(ctx.pins, static_cast<std::size_t>(ctx.pins.comparator()), pinHex);
        ImGui::SameLine(0.0f, 0.0f);
        if (ImGui::ColorButton("##picker-reference", pickerSwatchColor(ctx.pins.comparatorColor()),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(hero.heroWidth / 2.0f, hero.heroHeight))) {
            ImGui::SetClipboardText(pinHex);
        }
        ImGui::SetItemTooltip("pinned  %s - click to copy", pinHex);
        if (!hero.tiny) {
            draw->AddText(ImVec2(hero.heroOrigin.x + 5, hero.heroOrigin.y + 3), pickerLabelInk(ctx.color), "LIVE");
            const float pinLabel = ImGui::CalcTextSize("PIN").x;
            draw->AddText(ImVec2(hero.heroOrigin.x + hero.heroWidth - pinLabel - 5, hero.heroOrigin.y + 3),
                          pickerLabelInk(ctx.pins.comparatorColor()), "PIN");
            if (hero.onSwatch) {
                drawPickerSwatchBlock(draw, ctx, hero, ctx.color, true);
                drawPickerSwatchBlock(draw, ctx, hero, ctx.pins.comparatorColor(), false);
            }
        }
    }
    if (hero.soloOnSwatch) {
        drawPickerSoloReadout(draw, ctx, hero);
    }
}

// The same reading below the hero when the swatches are too small to carry it:
// the percent a photographer reads at a glance, with hex alongside.
void drawPickerValuesRow(const PickerContext& ctx, const ImVec2& area, float valuesStart)
{
    const float liveChannels[3] = {ctx.color.r, ctx.color.g, ctx.color.b};
    const char* channelLabels[3] = {"R", "G", "B"};
    for (int channel = 0; channel < 3; ++channel) {
        const float columnStart = valuesStart + channel * ctx.channelStride;
        if (channel > 0) {
            ImGui::SameLine(columnStart);
        } else {
            ImGui::SetCursorPosX(columnStart);
        }
        ImGui::TextUnformatted(channelLabels[channel]);
        char text[8];
        std::snprintf(text, sizeof(text), "%.0f%%", liveChannels[channel] / 2.55f);
        ImGui::SameLine(columnStart + ctx.labelColumn + ctx.columnGap + ctx.percentColumn -
                        ImGui::CalcTextSize(text).x);
        ImGui::TextUnformatted(text);
    }
    ImGui::SameLine(0.0f, 0.0f);
    pushHexFont(ctx.monospaceFont);
    if (ImGui::GetContentRegionAvail().x >= ctx.hexWidth + 12.0f) {
        ImGui::SameLine(area.x - ctx.hexWidth);
        ImGui::TextUnformatted(ctx.hex);
    } else {
        ImGui::NewLine();
        ImGui::TextUnformatted(ctx.hex);
    }
    popHexFont(ctx.monospaceFont);
}

// Three groups - lightness, chroma, hue - each a label over a fixed-width value.
// The group starts keep fixed strides so the letters never move.
void drawPickerDiffTriplet(const PickerContext& ctx, float valuesStart, const float diffValues[3],
                           float diffValueColumn)
{
    const char* diffLabels[3] = {"L", "C", "H"};
    const char* diffHelp = "live minus pinned: lightness, chroma, and hue weighted by chroma - sRGB assumed";
    float groupX = valuesStart;
    for (int component = 0; component < 3; ++component) {
        if (component == 0) {
            ImGui::SetCursorPosX(groupX);
        } else {
            ImGui::SameLine(groupX);
        }
        ImGui::TextDisabled("%s", diffLabels[component]);
        char value[8];
        std::snprintf(value, sizeof(value), "%+d", static_cast<int>(std::lround(diffValues[component])));
        const float labelWidth = ImGui::CalcTextSize(diffLabels[component]).x;
        ImGui::SameLine(groupX + labelWidth + ctx.columnGap);
        hexFontTextDisabled(ctx.monospaceFont, value);
        ImGui::SetItemTooltip("%s", diffHelp);
        groupX += labelWidth + ctx.columnGap + diffValueColumn + 2.0f * ctx.columnGap;
    }
}

// One quiet line below the hero: the colorist's difference on the left and a
// match percentage on the right. The triplet drops out whole when the line is
// too narrow to seat it clear of the match, which always stays.
void drawPickerDifferenceRow(const PickerContext& ctx, const ImVec2& area, float valuesStart)
{
    const LabColor liveLab = labFromSrgb(ctx.color);
    const LabColor pinLab = labFromSrgb(ctx.pins.comparatorColor());
    const ColorDifference difference = differenceFrom(pinLab, liveLab);
    // Match is 100 minus the CIEDE2000 distance, floored.
    const int matchPercent = static_cast<int>(std::clamp(100.0f - difference.deltaE, 0.0f, 100.0f));
    char matchValue[8];
    std::snprintf(matchValue, sizeof(matchValue), "%d%%", matchPercent);
    const float matchValueX = area.x - hexFontWidth(ctx.monospaceFont, matchValue);
    const float matchLabelX = matchValueX - ctx.columnGap - ImGui::CalcTextSize("Match").x;
    char matchHelp[192];
    std::snprintf(matchHelp, sizeof(matchHelp),
                  "similarity to the live color: 100%% is identical, 0%% is as far apart as black and white "
                  "(CIEDE2000 difference %.1f) - sRGB assumed",
                  difference.deltaE);
    const float diffValues[3] = {difference.lightness, difference.chroma, difference.hue};
    const float diffValueColumn = hexFontWidth(ctx.monospaceFont, "+199");
    const char* diffLabels[3] = {"L", "C", "H"};
    float tripletWidth = 0.0f;
    for (int component = 0; component < 3; ++component) {
        tripletWidth += ImGui::CalcTextSize(diffLabels[component]).x + ctx.columnGap + diffValueColumn;
        if (component < 2) {
            tripletWidth += 2.0f * ctx.columnGap;
        }
    }
    if (valuesStart + tripletWidth + ctx.columnGap <= matchLabelX) {
        drawPickerDiffTriplet(ctx, valuesStart, diffValues, diffValueColumn);
        ImGui::SameLine(matchLabelX);
    } else {
        ImGui::SetCursorPosX(matchLabelX);
    }
    ImGui::TextDisabled("Match");
    ImGui::SetItemTooltip("%s", matchHelp);
    ImGui::SameLine(matchValueX);
    hexFontText(ctx.monospaceFont, matchValue);
    ImGui::SetItemTooltip("%s", matchHelp);
}

// Progressive disclosure: Match first, then L/C/H, then R/G/B, each group
// admitted only if the whole block still clears the hex column.
void admitDeckGroups(float leftPartEnd, float deckWidth, float blockGap, float columnGap, float matchCol,
                     float lchGroupWidth, float rgbGroupWidth, DeckLayout& layout)
{
    float numericBlockWidth = 0.0f;
    const auto admitGroup = [&](float groupWidth) {
        const float tentative = numericBlockWidth + (numericBlockWidth > 0.0f ? 3.0f * columnGap : 0.0f) + groupWidth;
        if (leftPartEnd + blockGap + tentative <= deckWidth) {
            numericBlockWidth = tentative;

            return true;
        }

        return false;
    };
    layout.showMatch = admitGroup(matchCol);
    layout.showLch = layout.showMatch && admitGroup(lchGroupWidth);
    layout.showRgb = layout.showLch && admitGroup(rgbGroupWidth);
}

// Right edges of every visible column, walked left to right from the block's
// left edge. The block anchors just past the hex column, not the far edge.
void computeDeckRights(float leftPartEnd, float blockGap, float columnGap, float matchCol, const float lchCol[3],
                       const float rgbCol[3], DeckLayout& layout)
{
    float walk = leftPartEnd + blockGap;
    if (layout.showMatch) {
        layout.matchRight = walk + matchCol;
        walk = layout.matchRight;
    }
    if (layout.showLch) {
        walk += 3.0f * columnGap;
        for (int column = 0; column < 3; ++column) {
            if (column > 0) {
                walk += 2.0f * columnGap;
            }
            layout.lchRight[column] = walk + lchCol[column];
            walk = layout.lchRight[column];
        }
    }
    if (layout.showRgb) {
        walk += 3.0f * columnGap;
        for (int column = 0; column < 3; ++column) {
            if (column > 0) {
                walk += 2.0f * columnGap;
            }
            layout.rgbRight[column] = walk + rgbCol[column];
            walk = layout.rgbRight[column];
        }
    }
}

// Fixed left part: cross, swatch, hex; then the numeric columns, each sized for
// its widest value or its header, whichever is wider. Width decisions use the
// child's own content region, so the last column is not clipped under a bar.
DeckLayout computeDeckLayout(const PickerContext& ctx, float deckWidth)
{
    DeckLayout layout{};
    const float hexColumn = hexFontWidth(ctx.monospaceFont, "#DDDDDD");
    layout.swatchX = ctx.lineHeight + 3.0f * ctx.columnGap;
    layout.hexX = layout.swatchX + ctx.lineHeight + ctx.columnGap;
    const float leftPartEnd = layout.hexX + hexColumn;
    const float matchCol = std::max(hexFontWidth(ctx.monospaceFont, "100%"), ImGui::CalcTextSize("Match").x);
    const char* lchLabels[3] = {"L", "C", "H"};
    const char* rgbLabels[3] = {"R", "G", "B"};
    float lchCol[3];
    float rgbCol[3];
    for (int column = 0; column < 3; ++column) {
        lchCol[column] = std::max(hexFontWidth(ctx.monospaceFont, "+199"), ImGui::CalcTextSize(lchLabels[column]).x);
        rgbCol[column] = std::max(hexFontWidth(ctx.monospaceFont, "+100%"), ImGui::CalcTextSize(rgbLabels[column]).x);
    }
    const float lchGroupWidth = lchCol[0] + lchCol[1] + lchCol[2] + 2.0f * (2.0f * ctx.columnGap);
    const float rgbGroupWidth = rgbCol[0] + rgbCol[1] + rgbCol[2] + 2.0f * (2.0f * ctx.columnGap);
    const float blockGap = 3.0f * ctx.columnGap;
    admitDeckGroups(leftPartEnd, deckWidth, blockGap, ctx.columnGap, matchCol, lchGroupWidth, rgbGroupWidth, layout);
    computeDeckRights(leftPartEnd, blockGap, ctx.columnGap, matchCol, lchCol, rgbCol, layout);
    // Each header label centers over the ink of a typical value - a sign and two
    // digits, right-aligned - rather than over the column box.
    layout.lchTypical = hexFontWidth(ctx.monospaceFont, "+34");
    layout.rgbTypical = hexFontWidth(ctx.monospaceFont, "+34%");
    layout.matchTypical = hexFontWidth(ctx.monospaceFont, "77%");

    return layout;
}

void drawPickerDeckHeader(const DeckLayout& layout)
{
    // Nothing sits above the cross, swatch, or hex; the first admitted column
    // sets the cursor and the rest ride SameLine.
    bool firstHeader = true;
    const auto headerCell = [&](float colRight, float typicalWidth, const char* label, const char* tip) {
        const float headerX = colRight - (typicalWidth + ImGui::CalcTextSize(label).x) / 2.0f;
        if (firstHeader) {
            ImGui::SetCursorPosX(headerX);
            firstHeader = false;
        } else {
            ImGui::SameLine(headerX);
        }
        ImGui::TextDisabled("%s", label);
        ImGui::SetItemTooltip("%s", tip);
    };
    if (layout.showMatch) {
        headerCell(layout.matchRight, layout.matchTypical, "Match", PickerMatchTip);
    }
    if (layout.showLch) {
        headerCell(layout.lchRight[0], layout.lchTypical, "L", PickerLchTip);
        headerCell(layout.lchRight[1], layout.lchTypical, "C", PickerLchTip);
        headerCell(layout.lchRight[2], layout.lchTypical, "H", PickerLchTip);
    }
    if (layout.showRgb) {
        headerCell(layout.rgbRight[0], layout.rgbTypical, "R", PickerRgbTip);
        headerCell(layout.rgbRight[1], layout.rgbTypical, "G", PickerRgbTip);
        headerCell(layout.rgbRight[2], layout.rgbTypical, "B", PickerRgbTip);
    }
}

// The remove cross leads the row: a frameless glyph, quiet gray until hovered,
// red at the moment of intent. Draws inside the deck child's own draw list.
void drawDeckRowCross(std::size_t index, float lineHeight, int& removePin)
{
    char closeId[24];
    std::snprintf(closeId, sizeof(closeId), "##unpin-%d", static_cast<int>(index));
    if (ImGui::InvisibleButton(closeId, ImVec2(lineHeight, lineHeight))) {
        removePin = static_cast<int>(index);
    }
    ImGui::SetItemTooltip("remove this pin");
    const ImVec2 crossLo = ImGui::GetItemRectMin();
    const ImVec2 crossHi = ImGui::GetItemRectMax();
    const ImVec2 crossCenter = ImVec2((crossLo.x + crossHi.x) / 2.0f, (crossLo.y + crossHi.y) / 2.0f);
    const float arm = lineHeight * 0.17f;
    const ImU32 crossInk = ImGui::IsItemHovered() ? IM_COL32(235, 90, 90, 255) : IM_COL32(150, 150, 150, 180);
    ImDrawList* cross = ImGui::GetWindowDrawList();
    cross->AddLine(ImVec2(crossCenter.x - arm, crossCenter.y - arm), ImVec2(crossCenter.x + arm, crossCenter.y + arm),
                   crossInk, 1.4f);
    cross->AddLine(ImVec2(crossCenter.x - arm, crossCenter.y + arm), ImVec2(crossCenter.x + arm, crossCenter.y - arm),
                   crossInk, 1.4f);
}

// The swatch: click loads it into the comparator, right-click manages. Its
// selection ring fetches the deck child's draw list.
void drawDeckRowSwatch(const PickerContext& ctx, std::size_t index, const DeckLayout& layout)
{
    char pinId[24];
    std::snprintf(pinId, sizeof(pinId), "##pin-%d", static_cast<int>(index));
    const bool selected = static_cast<int>(index) == ctx.pins.comparator();
    ImGui::SameLine(layout.swatchX);
    if (ImGui::ColorButton(pinId, pickerSwatchColor(ctx.pins.color(index)),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(ctx.lineHeight, ctx.lineHeight))) {
        ctx.pins.selectComparator(selected ? -1 : static_cast<int>(index));
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ctx.pins.manage(static_cast<int>(index));
        ImGui::OpenPopup("##pinned-menu");
    }
    ImGui::SetItemTooltip(selected ? "click to unload from the comparator" : "click to compare against the live color");
    if (selected) {
        // A white ring inside a dark one reads on any pin color; the gold rim
        // vanished on skin tones.
        const ImVec2 lo = ImGui::GetItemRectMin();
        const ImVec2 hi = ImGui::GetItemRectMax();
        ImDrawList* ringDraw = ImGui::GetWindowDrawList();
        ringDraw->AddRect(ImVec2(lo.x - 1, lo.y - 1), ImVec2(hi.x + 1, hi.y + 1), IM_COL32(0, 0, 0, 220), 0.0f, 0,
                          2.0f);
        ringDraw->AddRect(lo, hi, IM_COL32(235, 235, 235, 235), 0.0f, 0, 1.5f);
    }
}

// Hex then the numeric columns, each value right-aligned in its column and
// vertically centered like the hex.
void drawDeckRowValues(const PickerContext& ctx, std::size_t index, const DeckLayout& layout, float rowPosY,
                       float textDrop)
{
    char pinHex[8];
    pinHexOf(ctx.pins, index, pinHex);
    ImGui::SameLine(layout.hexX);
    ImGui::SetCursorPosY(rowPosY + textDrop);
    pushHexFont(ctx.monospaceFont);
    ImGui::TextUnformatted(pinHex);
    popHexFont(ctx.monospaceFont);
    if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(pinHex);
    }
    ImGui::SetItemTooltip("click to copy");
    const ColorDifference pinDiff = differenceFrom(labFromSrgb(ctx.pins.color(index)), labFromSrgb(ctx.color));
    const auto numericCell = [&](float colRight, const char* value, const char* tip) {
        ImGui::SameLine(colRight - hexFontWidth(ctx.monospaceFont, value));
        ImGui::SetCursorPosY(rowPosY + textDrop);
        hexFontTextDisabled(ctx.monospaceFont, value);
        ImGui::SetItemTooltip("%s", tip);
    };
    if (layout.showMatch) {
        char match[8];
        std::snprintf(match, sizeof(match), "%d%%",
                      static_cast<int>(std::clamp(100.0f - pinDiff.deltaE, 0.0f, 100.0f)));
        char matchHelp[192];
        std::snprintf(matchHelp, sizeof(matchHelp),
                      "similarity to the live color: 100%% is identical, 0%% is as far apart as black and "
                      "white (CIEDE2000 difference %.1f) - sRGB assumed",
                      pinDiff.deltaE);
        numericCell(layout.matchRight, match, matchHelp);
    }
    if (layout.showLch) {
        const float lchValues[3] = {pinDiff.lightness, pinDiff.chroma, pinDiff.hue};
        for (int column = 0; column < 3; ++column) {
            char value[8];
            std::snprintf(value, sizeof(value), "%+d", static_cast<int>(std::lround(lchValues[column])));
            numericCell(layout.lchRight[column], value, PickerLchTip);
        }
    }
    if (layout.showRgb) {
        const float pinChannels[3] = {ctx.pins.color(index).r, ctx.pins.color(index).g, ctx.pins.color(index).b};
        const float liveChannels[3] = {ctx.color.r, ctx.color.g, ctx.color.b};
        for (int column = 0; column < 3; ++column) {
            char value[8];
            std::snprintf(value, sizeof(value), "%+.0f%%", (liveChannels[column] - pinChannels[column]) / 2.55f);
            numericCell(layout.rgbRight[column], value, PickerRgbTip);
        }
    }
}

void drawPickerDeckRow(const PickerContext& ctx, std::size_t index, const DeckLayout& layout, int& removePin)
{
    const float rowPosY = ImGui::GetCursorPosY();
    const float textDrop = (ctx.lineHeight - ImGui::GetFontSize()) / 2.0f;
    drawDeckRowCross(index, ctx.lineHeight, removePin);
    drawDeckRowSwatch(ctx, index, layout);
    drawDeckRowValues(ctx, index, layout, rowPosY, textDrop);
}

// The full reference deck when there is room: a scrolling child with a header
// and one row per pin.
void drawPickerDeck(const PickerContext& ctx, int& removePin)
{
    ImGui::BeginChild("##pin-deck", ImVec2(0, 0));
    const float deckWidth = ImGui::GetContentRegionAvail().x;
    const DeckLayout layout = computeDeckLayout(ctx, deckWidth);
    drawPickerDeckHeader(layout);
    for (std::size_t index = 0; index < ctx.pins.size(); ++index) {
        drawPickerDeckRow(ctx, index, layout, removePin);
    }
    drawPinnedMenu(ctx.pins);
    ImGui::EndChild();
}

// The chip rail when the pane is too small for the deck: swatches only, click to
// compare and right-click to manage.
void drawPickerChipRail(const PickerContext& ctx)
{
    for (std::size_t index = 0; index < ctx.pins.size(); ++index) {
        char pinId[24];
        std::snprintf(pinId, sizeof(pinId), "##pin-%d", static_cast<int>(index));
        char pinHex[8];
        pinHexOf(ctx.pins, index, pinHex);
        const bool selected = static_cast<int>(index) == ctx.pins.comparator();
        if (index > 0) {
            ImGui::SameLine(0.0f, 4.0f);
        }
        if (ImGui::ColorButton(pinId, pickerSwatchColor(ctx.pins.color(index)),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(ctx.lineHeight, ctx.lineHeight))) {
            ctx.pins.selectComparator(selected ? -1 : static_cast<int>(index));
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ctx.pins.manage(static_cast<int>(index));
            ImGui::OpenPopup("##pinned-menu");
        }
        ImGui::SetItemTooltip("%s - click to compare, right-click to manage", pinHex);
        if (selected) {
            const ImVec2 lo = ImGui::GetItemRectMin();
            const ImVec2 hi = ImGui::GetItemRectMax();
            ImDrawList* ringDraw = ImGui::GetWindowDrawList();
            ringDraw->AddRect(ImVec2(lo.x - 1, lo.y - 1), ImVec2(hi.x + 1, hi.y + 1), IM_COL32(0, 0, 0, 220), 0.0f, 0,
                              2.0f);
            ringDraw->AddRect(lo, hi, IM_COL32(235, 235, 235, 235), 0.0f, 0, 1.5f);
        }
    }
    drawPinnedMenu(ctx.pins);
}

// The color picker pane: the sampled cursor color as a large swatch with its
// values spelled out three ways at once - 0-255, percent, and hex - because
// matching a reference means never converting in your head. Clicking the swatch
// or the hex line copies the hex; the session's pinned colors (P) ride along.
// Three size tiers, few and spaced so resizing feels like deliberate steps: a
// strip, a compact comparator, the full reference deck. Order never changes -
// comparator, values, pins - and only the comparator absorbs extra height.
void drawColorPicker(const std::optional<FloatColor>& liveColor, PinBoard& pins, ImFont* monospaceFont)
{
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    if (pins.comparator() >= static_cast<int>(pins.size())) {
        pins.selectComparator(-1);
    }
    if (!liveColor) {
        drawPickerNoColor(area, lineHeight);

        return;
    }
    const FloatColor color = *liveColor;
    const int red = static_cast<int>(std::lround(std::clamp(color.r, 0.0f, 255.0f)));
    const int green = static_cast<int>(std::lround(std::clamp(color.g, 0.0f, 255.0f)));
    const int blue = static_cast<int>(std::lround(std::clamp(color.b, 0.0f, 255.0f)));
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", red, green, blue);
    // Every value owns a column sized for its widest form and right-aligns
    // inside it - the toolbar's cure for layouts that twitch as digits come and
    // go. Hex measures in the fixed-width font, one figure serving every half.
    const float labelColumn = ImGui::CalcTextSize("R").x;
    const float percentColumn = ImGui::CalcTextSize("100%").x;
    const float columnGap = ImGui::CalcTextSize(" ").x;
    const float channelStride = labelColumn + columnGap + percentColumn + 2 * columnGap;
    const float hexWidth = hexFontWidth(monospaceFont, hex);
    const PickerContext ctx{color,     pins,          monospaceFont, labelColumn, percentColumn,
                            columnGap, channelStride, hexWidth,      lineHeight,  hex};

    const PickerHero hero = computePickerHero(ctx, area, style);
    drawPickerHero(ctx, hero);
    if (!hero.onSwatch && !hero.soloOnSwatch) {
        drawPickerValuesRow(ctx, area, hero.valuesStart);
    }
    if (hero.split && !hero.tiny) {
        drawPickerDifferenceRow(ctx, area, hero.valuesStart);
    }
    if (pins.empty()) {
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.35f));
    int removePin = -1;
    if (hero.full) {
        drawPickerDeck(ctx, removePin);
    } else {
        drawPickerChipRail(ctx);
    }
    if (removePin >= 0) {
        pins.removeAt(static_cast<std::size_t>(removePin));
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

std::string shortcutLabel(const std::string& name)
{
    return name == "Escape" ? "Esc" : name;
}

void menuAction(std::vector<NativeMenuItem>& menu, const char* label, int id, bool checked, std::string shortcut = "")
{
    menu.push_back({NativeMenuItem::Kind::Action, label, id, checked, std::move(shortcut)});
}

void menuSeparator(std::vector<NativeMenuItem>& menu)
{
    menu.push_back({NativeMenuItem::Kind::Separator, "", -1, false, ""});
}

void menuSubmenu(std::vector<NativeMenuItem>& menu, const char* label)
{
    menu.push_back({NativeMenuItem::Kind::SubmenuBegin, label, -1, false, ""});
}

void menuEndSubmenu(std::vector<NativeMenuItem>& menu)
{
    menu.push_back({NativeMenuItem::Kind::SubmenuEnd, "", -1, false, ""});
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
      m_scopeRegistry(builtinModules()),
      m_view(m_scopeRegistry)
{
    m_screenSample = std::make_shared<ScreenSample>();
}

namespace {

// The border returns this long after the active window last moved and the
// grip released - what keeps a slow drag's sparse updates from flickering it.
constexpr double AttachMotionSettleSeconds = 0.2;

// The border label wears the window title (the filename, usually), capped so
// a pathological title cannot dwarf the region. Cut at a UTF-8 code point
// boundary, never inside a character.
std::string borderLabelFrom(const std::string& title, const std::string& fallback)
{
    const std::string& label = title.empty() ? fallback : title;
    // A sanity bound only: the platform draw truncates visually to fit.
    constexpr std::size_t MaxLabelBytes = 96;
    if (label.size() <= MaxLabelBytes) {
        return label;
    }
    std::size_t cut = MaxLabelBytes;
    while (cut > 0 && (static_cast<unsigned char>(label[cut]) & 0xC0) == 0x80) {
        --cut;
    }

    return label.substr(0, cut) + "...";
}

}  // namespace

bool App::init()
{
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

    observeSystemWake([this] { m_captureController->markStale(); });
    observeEscapeWithoutKeyWindow([this] { m_orphanEscape.store(true); });
    // A foreground switch reroutes the borders on the very next frame: the
    // wake beats the idle tick, and the zeroed probe refreshes the attached
    // draw's remembered window immediately.
    observeForegroundChanges([this] {
        m_nextExternalWindowProbe = 0.0;
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
    // No new face check starts once the loop is done, so drain the at-most one
    // still in flight before m_callbackState leaves scope: its detached thread
    // holds a pointer to it.
    while (m_callbackState.faceCheckRunning.load()) {
        std::this_thread::yield();
    }

    persistPreferences();
    unwatchWindowMotion();
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
    // Installed before the ImGui backend so it chains this callback instead of
    // being replaced by it. The chained call carries the same window, so the
    // state comes back through its user pointer.
    glfwSetWindowFocusCallback(m_window, [](GLFWwindow* focusTarget, int focused) {
        if (focused) {
            static_cast<AppCallbackState*>(glfwGetWindowUserPointer(focusTarget))->faceCheckRequested.store(true);
        }
    });
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
    m_uiScale = computeUiScale(m_window);
    if (m_uiScale != 1.0f) {
        applyUiScale(m_uiScale);
    }
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

void App::setupCapture()
{
    m_capture = createScreenCaptureSource();
    m_captureController.emplace(*m_capture, m_mailbox);
    // The display under this window's center: full-screen capture is a promise
    // about the screen the user can see the scopes on.
    if (m_captureController->requestPermission()) {
        m_captureController->requestDisplay(displayOfWindow().value_or(0));
        m_captureController->start();
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
    m_view.restoreStack(startup.scopeStack);
    m_view.setGraticule(startup.showGraticule);
    m_view.setZoom(startup.vectorscopeZoom);
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

void App::setWaveformStride(int stride)
{
    m_analysis.scopeParams[WaveformScopeId]["stride"] = stride;
    m_analysis.scopeParams[ParadeScopeId]["stride"] = stride;
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
    if (const auto custom = m_scopeShortcuts.find(std::string{id}); custom != m_scopeShortcuts.end()) {
        return custom->second;
    }
    const HostScope* scope = m_scopeRegistry.byId(id);

    return scope != nullptr && scope->letter != 0 ? std::string(1, scope->letter) : std::string{};
}

bool App::pinsAvailable() const
{
    // Pins mark the vectorscope and the color picker; without either on screen,
    // the tool's button, menu entries, and shortcuts all stand down together.
    return m_view.shows(VectorscopeScopeId) || m_view.shows(ColorPickerScopeId);
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

bool App::isFullRegion() const
{
    return m_analysis.region.leftPercent <= 0.0 && m_analysis.region.topPercent <= 0.0 &&
           m_analysis.region.rightPercent >= 100.0 && m_analysis.region.bottomPercent >= 100.0;
}

void App::syncRegionBorder()
{
    if (m_captureController->capturedDisplay() == 0) {
        return;
    }
    // The border shows only while this application is itself visible - a
    // hidden or minimized SideScopes must not leave regions floating on
    // screen - never during a pick or window motion. What it outlines
    // follows the focus routing already folded into the analysis region: the
    // attached region on the focused tracked window (label and warm dress),
    // else the plain global one. Called every frame; the platform side makes
    // the unchanged case free.
    if (m_regionPicking || isFullRegion() || applicationHidden() || m_attachedWindowMoving ||
        glfwGetWindowAttrib(m_window, GLFW_ICONIFIED)) {
        hideRegionBorder();
    } else {
        showRegionBorder(m_captureController->capturedDisplay(), m_analysis.region,
                         m_attachActiveWatched != 0 ? m_attachActiveLabel : std::string());
    }
}

std::optional<FloatColor> App::averageFrameArea(const RegionOfInterest& area) const
{
    // Averages a display-percent area of the latest frame: a dragged pin's
    // sample. A drag is the explicit request to average textured pixels; a
    // plain click samples a point instead, matching the live readout.
    std::optional<FloatColor> color;
    const bool sampled = m_worker.withLatestFrame([&](const FrameView& view) {
        const int left = std::clamp(static_cast<int>(area.leftPercent / 100.0 * view.width), 0, view.width);
        const int right = std::clamp(static_cast<int>(area.rightPercent / 100.0 * view.width), 0, view.width);
        const int top = std::clamp(static_cast<int>(area.topPercent / 100.0 * view.height), 0, view.height);
        const int bottom = std::clamp(static_cast<int>(area.bottomPercent / 100.0 * view.height), 0, view.height);
        if (right <= left || bottom <= top) {
            return;
        }
        // A stride caps the work on huge drags; the average barely moves past a
        // hundred thousand samples.
        const long long pixels = static_cast<long long>(right - left) * (bottom - top);
        const int stride = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(pixels) / 100000.0)));
        double sumR = 0;
        double sumG = 0;
        double sumB = 0;
        long long count = 0;
        for (int y = top; y < bottom; y += stride) {
            for (int x = left; x < right; x += stride) {
                const uint8_t* pixel = view.pixelAt(x, y);
                sumB += pixel[0];
                sumG += pixel[1];
                sumR += pixel[2];
                ++count;
            }
        }
        if (count == 0) {
            return;
        }
        color = FloatColor{static_cast<float>(sumR / count), static_cast<float>(sumG / count),
                           static_cast<float>(sumB / count)};
    });

    return sampled ? color : std::nullopt;
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

void App::resetRegionToFull()
{
    // Resets all selection: a pending pick, every tracked window, and the
    // global region alike. The border sync rides the analysis-dirty path.
    cancelRegionPick();
    if (m_attach.attached()) {
        m_attach.detachAll();
        unwatchWindowMotion();
        m_attachActiveWatched = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
    m_globalRegion = RegionOfInterest{};
    m_analysis.region = RegionOfInterest{};
    m_analysisDirty = true;
}

// Remember the window the user works in whenever another application holds
// the foreground: the attached draw (Shift+D) targets it after SideScopes
// has taken the keyboard to receive the shortcut.
void App::probeExternalWindow()
{
    if (glfwGetTime() < m_nextExternalWindowProbe) {
        return;
    }
    m_nextExternalWindowProbe = glfwGetTime() + 0.25;
    const int64_t externalPid = foregroundApplicationPid();
    if (externalPid == 0 || externalPid == m_ownPid) {
        return;
    }
    if (const auto externalWindow = frontmostWindowOfApplication(externalPid)) {
        m_lastExternalWindowId = *externalWindow;
        m_lastExternalOwnerPid = externalPid;
    }
}

// Gathers this frame's observation for every tracked window: geometry,
// minimized state, and - for visible ones - the display it sits on.
std::vector<TrackedWindowObservation> App::gatherTrackedObservations() const
{
    std::vector<TrackedWindowObservation> observations;
    for (const uint64_t identity : m_attach.trackedIdentities()) {
        TrackedWindowObservation observation;
        observation.identity = identity;
        const auto windowGeom = windowGeometry(identity);
        if (windowGeom) {
            observation.windowRect =
                AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height};
            observation.minimized = windowGeom->minimized;
            observation.title = windowGeom->title;
            if (!windowGeom->minimized) {
                const DesktopPoint centre{windowGeom->x + windowGeom->width / 2.0,
                                          windowGeom->y + windowGeom->height / 2.0};
                if (const auto displayId = displayAtPoint(centre)) {
                    if (const auto display = geometryOfDisplay(*displayId)) {
                        observation.displayId = *displayId;
                        observation.display = AttachDisplayRect{display->originX, display->originY,
                                                                display->widthPoints, display->heightPoints};
                    }
                }
            }
        }
        observations.push_back(observation);
    }

    return observations;
}

// Whether the active window's rectangle changed since the last follow step
// while staying the SAME window - real motion, which the border sits out. A
// change of active window is a switch instead: the border simply jumps along
// with the region.
bool App::activeWindowMoved(const AttachDecision& decision) const
{
    if (decision.activeIdentity == 0 || decision.activeIdentity != m_attachActiveWatched) {
        return false;
    }
    if (!m_attachLastSeenRect || !decision.activeRect) {
        return false;
    }

    return decision.activeRect->x != m_attachLastSeenRect->x || decision.activeRect->y != m_attachLastSeenRect->y ||
           decision.activeRect->width != m_attachLastSeenRect->width ||
           decision.activeRect->height != m_attachLastSeenRect->height;
}

// Follow the active window across displays: capture the one it now sits on,
// reusing the existing display-switch path.
void App::captureActiveDisplay(const AttachDecision& decision)
{
    if (decision.activeDisplayId != 0 && decision.activeDisplayId != m_captureController->capturedDisplay() &&
        m_captureController->permissionGranted() && !m_captureController->dead()) {
        m_captureController->requestDisplay(decision.activeDisplayId);
        m_captureController->start();
        m_lastActivity = glfwGetTime();
    }
}

// Applies a per-frame attach verdict - the whole focus routing in one line:
// the focused tracked window's region when there is one, the global region
// otherwise. Motion detection is the follow step's business - a region
// change here may equally be the active window switching, which must not
// blank the border.
void App::applyAttachDecision(const AttachDecision& decision)
{
    setRegion(decision.region ? *decision.region : m_globalRegion);
    if (decision.closedCount > 0) {
        m_attachDetachNotice = decision.detachedAll ? "window closed - detached" : "window closed - still tracking";
        m_attachNoticeUntil = glfwGetTime() + 5.0;
    }
    if (decision.detachedAll) {
        unwatchWindowMotion();
        m_attachActiveWatched = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
}

// The event-driven side of the border's motion reaction: delivered on the
// main thread by the platform watch the moment the user grips or moves the
// active window, even mid idle-wait, so the hide precedes the first stale
// composite instead of trailing it by a poll.
void App::onWindowMotion(WindowMotionSignal signal)
{
    switch (signal) {
    case WindowMotionSignal::GripDown:
        m_attachGripActive = true;
        // A click into a tracked window is often a focus change too: wake
        // the loop so the border's focus rule reacts promptly.
        glfwPostEmptyEvent();
        break;
    case WindowMotionSignal::MotionImminent:
    case WindowMotionSignal::Moved:
        m_attachRegionMovedAt = glfwGetTime();
        if (!m_attachedWindowMoving) {
            m_attachedWindowMoving = true;
            hideRegionBorder();
        }
        glfwPostEmptyEvent();
        break;
    case WindowMotionSignal::GripUp:
        m_attachGripActive = false;
        // The settle countdown starts at release, not at the last move a
        // poll happened to see.
        m_attachRegionMovedAt = glfwGetTime();
        glfwPostEmptyEvent();
        break;
    }
}

// One follow step: observes every tracked window, lets the controller pick
// the active one and map its region, and applies the verdict. Runs twice per
// frame - once before the frame, and again right after the swap so the
// border and region are repositioned from geometry read after the vsync
// wait, not a frame earlier.
void App::followAttachedWindow()
{
    if (!m_attach.attached() || m_regionPicking) {
        return;
    }

    // The focused window drives everything: the foreground application's
    // frontmost ordinary window - frozen on the active window while its
    // border is being dragged, and held while SideScopes itself is in
    // front. One region type at a time means there is no global region to
    // switch to while windows are tracked, so the user can work the scopes
    // against the last attached region without losing it.
    std::optional<uint64_t> focused;
    if (m_attachBorderEditing && m_attachActiveWatched != 0) {
        focused = m_attachActiveWatched;
    } else {
        const int64_t foreground = foregroundApplicationPid();
        if (foreground == m_ownPid && m_attachActiveWatched != 0) {
            focused = m_attachActiveWatched;
        } else {
            focused = frontmostWindowOfApplication(foreground);
        }
    }
    const AttachDecision decision = m_attach.observe(gatherTrackedObservations(), focused);
    if (activeWindowMoved(decision)) {
        onWindowMotion(WindowMotionSignal::Moved);
    }
    if (decision.activeIdentity != 0) {
        // The label prefers the window's live title - the filename in most
        // editors - and follows it when the window's content changes.
        m_attachActiveLabel = borderLabelFrom(decision.activeTitle, m_attach.activeApplicationName());
    }
    if (decision.activeIdentity != m_attachActiveWatched) {
        // The active window switched: the motion watch moves with it and the
        // border, no longer mid-anything, follows the routing right away.
        m_attachActiveWatched = decision.activeIdentity;
        m_attachGripActive = false;
        m_attachedWindowMoving = false;
        unwatchWindowMotion();
        if (m_attachActiveWatched != 0) {
            watchWindowMotion(m_attachActiveWatched, decision.activeOwnerPid,
                              [this](WindowMotionSignal signal) { onWindowMotion(signal); });
        }
        m_lastActivity = glfwGetTime();
    }
    m_attachLastSeenRect = decision.activeRect;
    applyAttachDecision(decision);
    captureActiveDisplay(decision);
    if (decision.closedCount > 0) {
        m_lastActivity = glfwGetTime();
    }
    // The window has sat still long enough and nothing grips it: the border
    // may come back where the motion left it.
    if (m_attachedWindowMoving && !m_attachGripActive &&
        glfwGetTime() - m_attachRegionMovedAt > AttachMotionSettleSeconds) {
        m_attachedWindowMoving = false;
    }
    syncRegionBorder();
}

// The idle tick, in slices, while windows are tracked: a programmatic window
// move (a snap tool - no mouse events, no move-size loop) still takes the
// border down within a slice instead of sitting stale for the whole tick,
// and a focus change with no event of ours (Cmd+` has no app-level
// notification) still reroutes within a slice. An early wake means a real
// event arrived; the frame body handles it now.
void App::idleWaitWatchingAttachedWindow()
{
    for (int slice = 0; slice < 4; ++slice) {
        const double sliceStart = glfwGetTime();
        glfwWaitEventsTimeout(0.025);
        if (glfwGetTime() - sliceStart < 0.023) {
            return;
        }
        const auto focusedNow = frontmostWindowOfApplication(foregroundApplicationPid());
        const uint64_t focusedTracked = focusedNow && m_attach.tracks(*focusedNow) ? *focusedNow : 0;
        if (focusedTracked != m_attachActiveWatched) {
            return;
        }
        if (m_attachActiveWatched == 0 || !m_attachLastSeenRect) {
            continue;
        }
        const auto rect = windowGeometry(m_attachActiveWatched);
        if (!rect) {
            return;  // closed: the frame body prunes it
        }
        if (rect->x != m_attachLastSeenRect->x || rect->y != m_attachLastSeenRect->y ||
            rect->width != m_attachLastSeenRect->width || rect->height != m_attachLastSeenRect->height) {
            onWindowMotion(WindowMotionSignal::Moved);

            return;
        }
    }
}

// A window rectangle as its display's percentages - the shape both the
// attached draw's constraint and the edit-time veil speak.
RegionOfInterest App::displayPercentRect(const WindowGeometry& windowGeom, const DisplayGeometry& display)
{
    RegionOfInterest region;
    region.leftPercent = std::clamp((windowGeom.x - display.originX) / display.widthPoints * 100.0, 0.0, 100.0);
    region.topPercent = std::clamp((windowGeom.y - display.originY) / display.heightPoints * 100.0, 0.0, 100.0);
    region.rightPercent =
        std::clamp((windowGeom.x + windowGeom.width - display.originX) / display.widthPoints * 100.0, 0.0, 100.0);
    region.bottomPercent =
        std::clamp((windowGeom.y + windowGeom.height - display.originY) / display.heightPoints * 100.0, 0.0, 100.0);

    return region;
}

// Resolves the attached draw's target and constraint at pick-open time: the
// last external window's current rectangle, in its display's percentages,
// plus the application name off the picker's own window list. Returns
// nothing - and the pick falls back to a plain global draw - when that
// window is gone or not visible.
std::optional<PickConstraint> App::makeAttachedDrawConstraint()
{
    m_attachedDrawTarget.reset();
    if (m_lastExternalWindowId == 0) {
        return std::nullopt;
    }
    const auto windowGeom = windowGeometry(m_lastExternalWindowId);
    if (!windowGeom || windowGeom->minimized) {
        return std::nullopt;
    }
    const DesktopPoint centre{windowGeom->x + windowGeom->width / 2.0, windowGeom->y + windowGeom->height / 2.0};
    const auto displayId = displayAtPoint(centre);
    if (!displayId) {
        return std::nullopt;
    }
    const auto display = geometryOfDisplay(*displayId);
    if (!display) {
        return std::nullopt;
    }

    PickConstraint constraint;
    constraint.displayId = *displayId;
    constraint.region = displayPercentRect(*windowGeom, *display);
    std::string application;
    for (const DesktopWindow& candidate : onScreenWindows(*displayId)) {
        if (candidate.windowIdentity == m_lastExternalWindowId) {
            application = candidate.application;
            break;
        }
    }
    constraint.label = borderLabelFrom(windowGeom->title, application);
    m_attachedDrawTarget = AttachedDrawTarget{m_lastExternalWindowId, m_lastExternalOwnerPid, constraint.label};

    return constraint;
}

// Sheds only the front tracked window; the last one's detach is the full
// reset back to the screen.
void App::stopTrackingActiveWindow()
{
    if (m_attach.trackedCount() > 1 && m_attach.activeIdentity() != 0) {
        m_attach.remove(m_attach.activeIdentity());
        unwatchWindowMotion();
        m_attachActiveWatched = 0;
    } else {
        resetRegionToFull();
    }
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
    preferences.shortcuts = m_shortcuts;
    preferences.scopeShortcuts = m_scopeShortcuts;
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
    drainAsyncSignals();
    // Capture is a service that dies (lock screen, display sleep); restarting
    // it is our job.
    m_captureController->service(glfwGetTime());
    probeExternalWindow();
    // Attached regions: observe the tracked windows and route the analysis by
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
        m_lastActivity = glfwGetTime();
    }
    m_frameSize = m_worker.latestFrameSize();
    publishSelfWindowMask();
    sampleCursorColor();
    updateAdaptiveDetail(framebufferWidth);

    drawFrameUi();

    ImGui::Render();
    m_graphics->endFrame();

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

void App::pumpEvents()
{
    // Idle: with no new output, no cursor motion, and no interaction, wait for
    // events at a slow tick instead of spinning at refresh - in short slices
    // while windows are tracked, so their motion and focus stay fresh.
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
    if (m_callbackState.faceCheckRequested.exchange(false)) {
        refreshFacePresence(m_worker, m_captureController->capturedDisplay(), m_callbackState);
    }
    if (m_callbackState.iconifyChanged.exchange(false)) {
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
    if (m_orphanEscape.exchange(false)) {
        resetRegionToFull();
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
    // With no region drawn and no window tracked, capture follows the display
    // this window sits on. A drawn region or a tracked window pins capture to
    // its own display regardless of the window.
    if (m_captureController->permissionGranted() && !m_captureController->dead() && !m_regionPicking &&
        isFullRegion() && !m_attach.attached()) {
        const auto homeDisplay = displayOfWindow();
        if (homeDisplay && *homeDisplay != m_captureController->capturedDisplay()) {
            m_captureController->requestDisplay(*homeDisplay);
            if (m_captureController->start()) {
                m_lastActivity = glfwGetTime();
            }
        }
    }
}

void App::syncUiScaleToMonitor()
{
    // The window may have moved to a monitor with a different scale.
    const float currentScale = computeUiScale(m_window);
    if (currentScale != m_uiScale) {
        applyUiScale(currentScale);
        m_lastActivity = glfwGetTime();
    }
}

void App::publishSelfWindowMask()
{
    // Publish our own window rectangle (frame pixels, generous chrome margins)
    // so analysis masks it out of change detection.
    if (!m_frameSize || m_captureController->capturedDisplay() == 0) {
        return;
    }
    const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay());
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
    const IntRect selfWindow{static_cast<int>((windowX - geometry->originX - 8 * m_uiScale) * scaleX),
                             static_cast<int>((windowY - geometry->originY - 42 * m_uiScale) * scaleY),
                             static_cast<int>((windowW + 16 * m_uiScale) * scaleX),
                             static_cast<int>((windowH + 58 * m_uiScale) * scaleY)};
    if (selfWindow.x != m_analysis.maskedWindow.x || selfWindow.y != m_analysis.maskedWindow.y ||
        selfWindow.width != m_analysis.maskedWindow.width || selfWindow.height != m_analysis.maskedWindow.height) {
        m_analysis.maskedWindow = selfWindow;
        m_analysisDirty = true;
    }
}

void App::sampleCursorColor()
{
    // Cursor color, smoothed per scope with its own rhythm. On the tracked
    // display it reads the capture stream's frame; on every other display a
    // throttled one-shot sample keeps the readout alive even while capture is
    // paused.
    if (m_captureController->capturedDisplay() == 0) {
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
    const bool onTrackedDisplay = displayAtPoint(*cursor).value_or(0) == m_captureController->capturedDisplay();
    if (onTrackedDisplay && !m_captureController->dead() && m_frameSize) {
        if (const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay())) {
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
    const float density = windowW > 0 ? static_cast<float>(framebufferWidth) / windowW : 1.0f;
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
    drawCursorReadout();
    drawScopePanes();
    handleContextMenu();

    ImGui::End();
    ImGui::PopStyleVar();

    drawSettingsWindow();
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
         {m_shortcuts.pickWindow, m_shortcuts.drawRegion, m_shortcuts.pickFaces, m_shortcuts.pinColor}) {
        if (shortcutPressed(binding)) {
            triggerShortcut(binding, modifiers.shift);
        }
    }
    handleViewShortcuts();
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
    if (key == m_shortcuts.pickWindow) {
        m_wantRegionPick = RegionPickerMode::PickWindows;
    } else if (key == m_shortcuts.drawRegion) {
        // Plain draw sets the global region; with Shift the draw is
        // constrained to - and attaches to - the last external window (the
        // one focused before SideScopes was).
        m_wantRegionPick = RegionPickerMode::Draw;
        m_wantAttachedDraw = shift;
    } else if (key == m_shortcuts.pickFaces && supportsFaceDetection()) {
        m_wantRegionPick = RegionPickerMode::PickFaces;
    } else if (key == m_shortcuts.pinColor && pinsAvailable()) {
        // One pin tool; each click inside decides between pin-and-close and
        // Shift's pin-and-continue.
        m_wantRegionPick = RegionPickerMode::PinColor;
    } else if (key == m_shortcuts.vectorscopeZoom) {
        m_view.setZoom(m_view.zoom() >= 4 ? 1 : m_view.zoom() * 2);
    } else if (key == m_shortcuts.fullRegion) {
        resetRegionToFull();
    } else {
        return false;
    }

    return true;
}

void App::handleViewShortcuts()
{
    if (shortcutPressed(m_shortcuts.vectorscopeZoom)) {
        m_view.setZoom(m_view.zoom() >= 4 ? 1 : m_view.zoom() * 2);
    }
    if (shortcutPressed(m_shortcuts.fullRegion)) {
        // Escape peels back one layer at a time: the settings window first, the
        // drawn region only when nothing is stacked above it.
        if (m_showSettings) {
            m_showSettings = false;
        } else {
            resetRegionToFull();
        }
    }
}

void App::drawRegionToolIcons()
{
    char tooltip[96];
    std::snprintf(tooltip, sizeof(tooltip), "Draw an area (%s) - Shift draws a region attached to a window",
                  m_shortcuts.drawRegion.c_str());
    if (iconButton("##draw-region", RegionIcon::Crosshair, tooltip)) {
        m_wantRegionPick = RegionPickerMode::Draw;
        m_wantAttachedDraw = ImGui::GetIO().KeyShift;
    }
    ImGui::SameLine(0.0f, 2.0f);
    std::snprintf(tooltip, sizeof(tooltip), "Pick a window (%s)", m_shortcuts.pickWindow.c_str());
    if (iconButton("##pick-region", RegionIcon::PickHand, tooltip)) {
        m_wantRegionPick = RegionPickerMode::PickWindows;
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (pinsAvailable()) {
        std::snprintf(tooltip, sizeof(tooltip), "Pin a color (%s) - Shift+click a color to pin several",
                      m_shortcuts.pinColor.c_str());
        if (iconButton("##pin-color", RegionIcon::Dropper, tooltip)) {
            m_wantRegionPick = RegionPickerMode::PinColor;
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    // The face button sits last among the pickers: it is the one most often
    // dimmed, and a disabled button reads best at the row's edge.
    if (supportsFaceDetection()) {
        const bool noneFound = m_callbackState.facesOnScreen.load() == 0;
        std::snprintf(tooltip, sizeof(tooltip), "Pick a face (%s)%s", m_shortcuts.pickFaces.c_str(),
                      noneFound ? " - none on screen right now" : "");
        if (iconButton("##pick-face", RegionIcon::Face, tooltip, noneFound)) {
            m_wantRegionPick = RegionPickerMode::PickFaces;
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    if (!isFullRegion()) {
        if (iconButton("##full-region", RegionIcon::Expand, "Reset to full screen (Esc)")) {
            resetRegionToFull();
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    // The attached regions identify themselves on their own borders; the
    // toolbar keeps only the brief note after a tracked window closed out
    // from under its region.
    if (glfwGetTime() < m_attachNoticeUntil) {
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextDisabled("%s", m_attachDetachNotice.c_str());
    }
}

void App::drawCursorReadout()
{
    // The readout yields before the icons do: on a narrow window it would
    // right-align on top of the toolbar buttons, and the buttons win.
    const float toolbarEnd = ImGui::GetCursorPosX();
    if (!m_vectorscopeColor) {
        ImGui::NewLine();

        return;
    }
    const FloatColor& color = *m_vectorscopeColor;
    // Each value gets a column sized for the widest it can be and is
    // right-aligned inside it, so neither swatch nor numbers wander.
    const float columnWidth = ImGui::CalcTextSize("100%").x;
    const float columnGap = ImGui::CalcTextSize(" ").x;
    const float swatch = ImGui::GetTextLineHeight();
    const float textWidth = 3 * columnWidth + 2 * columnGap;
    const float readoutStart = ImGui::GetWindowContentRegionMax().x - (textWidth + swatch + 6);
    if (readoutStart < toolbarEnd + 8) {
        ImGui::NewLine();

        return;
    }
    ImGui::SameLine(readoutStart);
    ImGui::ColorButton("##cursor-color", ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(swatch, swatch));
    const float columnsStart = readoutStart + swatch + 6;
    const float channels[3] = {color.r, color.g, color.b};
    for (int channel = 0; channel < 3; ++channel) {
        char value[8];
        std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55);
        const float columnStart = columnsStart + channel * (columnWidth + columnGap);
        ImGui::SameLine(columnStart + columnWidth - ImGui::CalcTextSize(value).x);
        ImGui::TextUnformatted(value);
    }
}

void App::drawScopePanes()
{
    // The enabled scopes stack in a fixed order, splitting the window along its
    // longer axis. A scope's pane point and rect live at its own identity index,
    // so the adaptive block and the context menu read back the right pane.
    m_paneRects.assign(m_scopeRegistry.scopes().size(), ImVec4());
    const int paneCount = static_cast<int>(m_view.stack().size());
    const ImVec2 area = ImGui::GetContentRegionAvail();
    if (!m_captureController->permissionGranted()) {
        drawCaptureHelp("SideScopes cannot see the screen",
                        {
                            "macOS requires the Screen Recording permission.",
                            "",
                            "1. Click the button below",
                            "2. Turn on SideScopes in the list",
                            "3. Quit and reopen SideScopes",
                        },
                        true);
    } else if (m_captureController->dead()) {
        const std::string status = m_captureController->status();
        drawCaptureHelp("Screen capture was interrupted", {status, "Reconnecting automatically..."}, false);
    } else if (paneCount <= 1) {
        if (paneCount == 1) {
            drawScopeById(m_view.stack().front());
        }
    } else {
        const bool horizontal = area.x >= area.y;
        const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
        const ImVec2 paneSize = horizontal ? ImVec2((area.x - spacing.x * (paneCount - 1)) / paneCount, area.y)
                                           : ImVec2(area.x, (area.y - spacing.y * (paneCount - 1)) / paneCount);
        for (int pane = 0; pane < paneCount; ++pane) {
            ImGui::BeginChild(m_paneIds[static_cast<std::size_t>(pane)].c_str(), paneSize);
            drawScopeById(m_view.stack()[static_cast<std::size_t>(pane)]);
            ImGui::EndChild();
            if (horizontal && pane + 1 < paneCount) {
                ImGui::SameLine();
            }
        }
    }
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

void App::drawSettingsWindow()
{
    if (!m_showSettings) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Settings", &m_showSettings, ImGuiWindowFlags_NoCollapse);
    ImGui::TextWrapped("capture: %s", m_captureController->status().c_str());
    ImGui::Text("analysis %.2f ms | frames %llu | ui %.0f fps", m_output.accumulateMilliseconds,
                static_cast<unsigned long long>(m_output.framesProcessed), static_cast<double>(io.Framerate));
    ImGui::Separator();
    drawVectorscopeSettings();
    drawWaveformSettings();
    ImGui::TextDisabled("modes and toggles: right-click a scope");
    ImGui::TextDisabled("%s", m_versionInfo.display.c_str());
    ImGui::End();
}

void App::drawVectorscopeSettings()
{
    // The intensity and stride sliders read their default, headroom, and range
    // from the descriptor; the smoothing slider is host state.
    const SsParamInfo* vectorscopeGain = firstParamOfKind(descriptorFor(VectorscopeScopeId), SS_PARAM_INTENSITY);
    const SsParamInfo* vectorscopeStrideParam = firstParamOfKind(descriptorFor(VectorscopeScopeId), SS_PARAM_INT);
    ImGui::TextDisabled("vectorscope");
    float vectorscopePercent = m_view.intensity(VectorscopeScopeId);
    if (ImGui::SliderFloat("intensity##v", &vectorscopePercent, 0.0f, 100.0f, "%.0f%%")) {
        m_view.setIntensity(VectorscopeScopeId, vectorscopePercent);
        m_analysis.scopeParams[VectorscopeScopeId][vectorscopeGain->key] =
            traceGainFromIntensity(vectorscopePercent, static_cast<float>(vectorscopeGain->intensity_shift));
        m_analysisDirty = true;
    }
    int vectorscopeStride = static_cast<int>(
        scopeParam(VectorscopeScopeId, vectorscopeStrideParam->key, vectorscopeStrideParam->default_value));
    if (ImGui::SliderInt("sampling 1:N##v", &vectorscopeStride, static_cast<int>(vectorscopeStrideParam->min_value),
                         static_cast<int>(vectorscopeStrideParam->max_value))) {
        m_analysis.scopeParams[VectorscopeScopeId][vectorscopeStrideParam->key] = vectorscopeStride;
        m_analysisDirty = true;
    }
    float vectorscopeMs = m_view.smoothing(VectorscopeScopeId);
    if (ImGui::SliderFloat("smoothing ms##v", &vectorscopeMs, 0.0f, 500.0f, "%.0f")) {
        m_view.setSmoothing(VectorscopeScopeId, vectorscopeMs);
    }
}

void App::drawWaveformSettings()
{
    // The waveform and its parade share one control, so only the waveform is
    // shown.
    const SsParamInfo* waveformGain = firstParamOfKind(descriptorFor(WaveformScopeId), SS_PARAM_INTENSITY);
    const SsParamInfo* waveformStrideParam = firstParamOfKind(descriptorFor(WaveformScopeId), SS_PARAM_INT);
    ImGui::TextDisabled("waveform");
    float waveformPercent = m_view.intensity(WaveformScopeId);
    if (ImGui::SliderFloat("intensity##w", &waveformPercent, 0.0f, 100.0f, "%.0f%%")) {
        m_view.setIntensity(WaveformScopeId, waveformPercent);
        setWaveformGain(traceGainFromIntensity(waveformPercent, static_cast<float>(waveformGain->intensity_shift)));
        m_analysisDirty = true;
    }
    int waveformStride =
        static_cast<int>(scopeParam(WaveformScopeId, waveformStrideParam->key, waveformStrideParam->default_value));
    if (ImGui::SliderInt("sampling 1:N##w", &waveformStride, static_cast<int>(waveformStrideParam->min_value),
                         static_cast<int>(waveformStrideParam->max_value))) {
        setWaveformStride(waveformStride);
        m_analysisDirty = true;
    }
    float waveformMs = m_view.smoothing(WaveformScopeId);
    if (ImGui::SliderFloat("smoothing ms##w", &waveformMs, 0.0f, 500.0f, "%.0f")) {
        m_view.setSmoothing(WaveformScopeId, waveformMs);
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
    ImGui::SetItemTooltip("click to copy");
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
        ImGui::SetItemTooltip("%s", url);
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
    buildContextMenu(clickedPane, menu, paramActions);
    const int chosen = showNativeContextMenu(menu);
    dispatchMenuChoice(chosen, paramActions);
}

const std::map<std::string, double>& App::paramsOf(std::string_view id) const
{
    static const std::map<std::string, double> noParams;
    const auto stored = m_analysis.scopeParams.find(std::string{id});

    return stored != m_analysis.scopeParams.end() ? stored->second : noParams;
}

bool App::scopeHasOptions(std::string_view id) const
{
    if (id == VectorscopeScopeId || id == ColorPickerScopeId) {
        return true;  // host sections: zoom and pins
    }
    const HostScope* hostScope = m_scopeRegistry.byId(id);

    return hostScope != nullptr && hostScope->descriptor != nullptr &&
           firstParamOfKind(hostScope->descriptor, SS_PARAM_CHOICE) != nullptr;
}

void App::appendPinOptions(std::vector<NativeMenuItem>& menu)
{
    // Pins are a scope tool: they mark the vectorscope and the color picker, so
    // their submenu rides those scopes' own sections.
    menuSubmenu(menu, "Pins");
    menuAction(menu, "Pin Colors...", MenuPickPinColor, false, shortcutLabel(m_shortcuts.pinColor));
    if (!m_pins.empty()) {
        menuAction(menu, "Clear Pinned Markers", MenuClearPinnedMarkers, false);
    }
    menuEndSubmenu(menu);
}

void App::appendZoomOptions(std::vector<NativeMenuItem>& menu)
{
    // The vectorscope's magnify viewport is a host control, not a module
    // parameter, so it stays hand-built beside the descriptor choices.
    menuSubmenu(menu, "Zoom");
    menuAction(menu, "1x", MenuZoom1, m_view.zoom() == 1, shortcutLabel(m_shortcuts.vectorscopeZoom));
    menuAction(menu, "2x", MenuZoom2, m_view.zoom() == 2, shortcutLabel(m_shortcuts.vectorscopeZoom));
    menuAction(menu, "4x", MenuZoom4, m_view.zoom() == 4, shortcutLabel(m_shortcuts.vectorscopeZoom));
    menuEndSubmenu(menu);
}

void App::appendScopeOptions(std::string_view id, bool flatten, std::vector<NativeMenuItem>& menu,
                             std::vector<ParamMenuAction>& paramActions)
{
    // A scope's own options: its descriptor's choice submenus, then any host
    // sections it carries. `flatten` lets a lone choice sit directly under an
    // enclosing scope-name submenu.
    const HostScope* hostScope = m_scopeRegistry.byId(id);
    if (hostScope != nullptr && hostScope->descriptor != nullptr) {
        appendScopeChoiceMenus(*hostScope->descriptor, paramsOf(id), flatten, menu, paramActions);
    }
    if (id == VectorscopeScopeId) {
        appendZoomOptions(menu);
        appendPinOptions(menu);
    } else if (id == ColorPickerScopeId) {
        appendPinOptions(menu);
    }
}

void App::appendScopesSubmenu(std::vector<NativeMenuItem>& menu)
{
    menuSubmenu(menu, "Scopes");
    menuAction(menu, "Vectorscope", MenuShowVectorscope, m_view.shows(VectorscopeScopeId),
               shortcutLabel(bindingFor(VectorscopeScopeId)));
    menuAction(menu, "Waveform", MenuShowWaveform, m_view.shows(WaveformScopeId),
               shortcutLabel(bindingFor(WaveformScopeId)));
    menuAction(menu, "RGB Parade", MenuShowWaveformParade, m_view.shows(ParadeScopeId),
               shortcutLabel(bindingFor(ParadeScopeId)));
    menuAction(menu, "Histogram", MenuShowHistogram, m_view.shows(HistogramScopeId),
               shortcutLabel(bindingFor(HistogramScopeId)));
    menuAction(menu, "Color Picker", MenuShowColorPicker, m_view.shows(ColorPickerScopeId),
               shortcutLabel(bindingFor(ColorPickerScopeId)));
    menuEndSubmenu(menu);
}

void App::appendPerScopeOptions(std::vector<NativeMenuItem>& menu, std::vector<ParamMenuAction>& paramActions)
{
    // On a background or toolbar click, each visible scope's options ride under
    // its own name, in toolbar order.
    for (const HostScope& scope : m_scopeRegistry.scopes()) {
        if (!m_view.shows(scope.id)) {
            continue;
        }
        // The vectorscope's section already carries the pins; the color picker
        // shows them only when the vectorscope is gone.
        if (scope.id == ColorPickerScopeId && m_view.shows(VectorscopeScopeId)) {
            continue;
        }
        if (!scopeHasOptions(scope.id)) {
            continue;  // the parade offers no options of its own
        }
        menuSubmenu(menu, scope.descriptor != nullptr ? scope.descriptor->name : "Color Picker");
        appendScopeOptions(scope.id, true, menu, paramActions);
        menuEndSubmenu(menu);
    }
}

void App::appendRegionAndAppSection(std::vector<NativeMenuItem>& menu)
{
    menuSeparator(menu);
    menuAction(menu, "Pick Window or Photo...", MenuSelectRegion, false, shortcutLabel(m_shortcuts.pickWindow));
    menuAction(menu, "Draw Area...", MenuDrawRegion, false, shortcutLabel(m_shortcuts.drawRegion));
    if (supportsFaceDetection()) {
        menuAction(menu, "Find Faces...", MenuPickFaces, false, shortcutLabel(m_shortcuts.pickFaces));
    }
    menuAction(menu, "Watch Full Screen", MenuFullScreenRegion, isFullRegion(), shortcutLabel(m_shortcuts.fullRegion));
    if (m_attach.trackedCount() > 1) {
        if (m_attach.activeIdentity() != 0) {
            menuAction(menu, "Stop Tracking Front Window", MenuDetachWindow, false);
        }
        menuAction(menu, "Detach All Windows", MenuDetachAll, false);
    } else if (m_attach.attached()) {
        menuAction(menu, "Detach from Window", MenuDetachWindow, false);
    }

    menuSeparator(menu);
    menuAction(menu, "Graticule", MenuToggleGraticule, m_view.graticule());

    menuSeparator(menu);
    menuAction(menu, "About SideScopes", MenuAbout, false);
    menuAction(menu, "Settings...", MenuOpenSettings, false);
    menuAction(menu, "Quit", MenuQuit, false);
}

void App::buildContextMenu(int clickedPane, std::vector<NativeMenuItem>& menu,
                           std::vector<ParamMenuAction>& paramActions)
{
    // One rule shapes the menu: ownership shows through position and grouping.
    // The clicked pane's options lead, unprefixed - the click is the context; a
    // background or toolbar click wraps each scope's options in its own submenu.
    if (clickedPane >= 0) {
        const std::string& clickedId = m_scopeRegistry.scopes()[static_cast<std::size_t>(clickedPane)].id;
        if (scopeHasOptions(clickedId)) {
            appendScopeOptions(clickedId, false, menu, paramActions);
            menuSeparator(menu);
        }
    }
    appendScopesSubmenu(menu);
    if (clickedPane < 0) {
        appendPerScopeOptions(menu, paramActions);
    }
    appendRegionAndAppSection(menu);
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
    case MenuSelectRegion:
        m_wantRegionPick = RegionPickerMode::PickWindows;
        break;
    case MenuDrawRegion:
        m_wantRegionPick = RegionPickerMode::Draw;
        break;
    case MenuPickFaces:
        m_wantRegionPick = RegionPickerMode::PickFaces;
        break;
    case MenuFullScreenRegion:
    case MenuDetachAll:
        resetRegionToFull();
        break;
    case MenuDetachWindow:
        stopTrackingActiveWindow();
        break;
    case MenuPickPinColor:
        m_wantRegionPick = RegionPickerMode::PinColor;
        break;
    case MenuClearPinnedMarkers:
        m_pins.clear();
        break;
    default:
        break;
    }
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

void App::handleRegionPicking()
{
    if (m_regionPicking && m_wantRegionPick) {
        const bool targetPin = *m_wantRegionPick == RegionPickerMode::PinColor;
        if (targetPin || m_regionPickIsPin) {
            // Region picking and color pinning are separate tools; a pick never
            // morphs across that boundary. The active pick closes with no effect
            // on the region, and the requested tool opens fresh once it is gone.
            m_regionPickSwallowCancel = true;
            cancelRegionPick();
        } else {
            // The toolbar keeps working mid-pick: choosing a region tool
            // switches the active picker's mode instead of stacking one.
            setRegionPickMode(*m_wantRegionPick);
            m_wantRegionPick.reset();
        }
    }
    if (!m_regionPicking && m_wantRegionPick && m_captureController->capturedDisplay() != 0) {
        openRegionPicker();
    }
}

void App::openRegionPicker()
{
    hideRegionBorder();
    // The previous region's border must not leak into the analyzed frame: its
    // strokes read as rectangle edges and cut suggestions short. Wait briefly
    // for a frame taken after the border left the screen.
    if (!isFullRegion()) {
        waitForBorderFreeFrame();
    }
    const std::vector<PickerDisplay> pickerDisplays = buildPickerDisplays();
    dumpSuggestionsIfRequested(pickerDisplays);
    std::optional<PickConstraint> constraint;
    if (m_wantAttachedDraw && *m_wantRegionPick == RegionPickerMode::Draw) {
        constraint = makeAttachedDrawConstraint();
    } else {
        m_attachedDrawTarget.reset();
    }
    m_wantAttachedDraw = false;
    if (beginRegionPick(pickerDisplays, *m_wantRegionPick, constraint)) {
        m_regionPicking = true;
        m_regionPickIsPin = *m_wantRegionPick == RegionPickerMode::PinColor;
    } else {
        m_attachedDrawTarget.reset();
    }
    // Consumed either way: a request that could not open must not retry every
    // frame.
    m_wantRegionPick.reset();
    m_lastActivity = glfwGetTime();
}

void App::waitForBorderFreeFrame()
{
    // The 60 ms floor outlasts an in-flight pre-hide frame's capture-to-delivery;
    // the 300 ms cap keeps the picker responsive if the stream has stalled.
    uint64_t staleSequence = 0;
    (void)m_worker.withLatestFrame([&](const FrameView& view) { staleSequence = view.sequence; });
    const double hiddenAt = glfwGetTime();
    for (;;) {
        const double elapsed = glfwGetTime() - hiddenAt;
        if (elapsed >= 0.3) {
            break;
        }
        uint64_t sequence = staleSequence;
        (void)m_worker.withLatestFrame([&](const FrameView& view) { sequence = view.sequence; });
        // Inequality, not greater-than: a freshly switched stream counts its
        // frames from one again.
        if (sequence != staleSequence && elapsed >= 0.06) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

std::vector<PickerDisplay> App::buildPickerDisplays()
{
    // The offer, per display: the visible application windows, frontmost first,
    // plus, behind their own key, the faces the platform detector finds in the
    // current frame. Faces are offered on the tracked display only.
    std::vector<SuggestedRegion> faceSuggestions;
    if (supportsFaceDetection()) {
        (void)m_worker.withLatestFrame([&](const FrameView& view) {
            const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay());
            const float pixelsPerPoint = geometry ? static_cast<float>(view.width / geometry->widthPoints) : 1.0f;
            faceSuggestions = buildFaceSuggestions(detectFaces(view, pixelsPerPoint), view.width, view.height);
            m_callbackState.facesOnScreen.store(static_cast<int>(faceSuggestions.size()));
        });
    }

    std::vector<PickerDisplay> pickerDisplays;
    // Remember every window and its identity so a confirmed window pick can
    // be turned into an attachment.
    m_pickableWindows.clear();
    for (const CaptureTarget& target : m_capture->listTargets()) {
        PickerDisplay entry;
        entry.displayId = target.displayId;
        entry.windows = windowSuggestionsFor(target.displayId);
        if (const auto geometry = geometryOfDisplay(target.displayId)) {
            for (const DesktopWindow& pickable : onScreenWindows(target.displayId)) {
                const WindowGeometry rect{pickable.x, pickable.y, pickable.width, pickable.height, false, {}};
                m_pickableWindows.push_back({pickable.windowIdentity, pickable.ownerPid, pickable.application,
                                             AttachWindowRect{pickable.x, pickable.y, pickable.width, pickable.height},
                                             displayPercentRect(rect, *geometry), target.displayId});
            }
        }
        if (target.displayId == m_captureController->capturedDisplay()) {
            entry.faces = faceSuggestions;
        }
        pickerDisplays.push_back(std::move(entry));
    }

    return pickerDisplays;
}

// Recovers which pickable window a confirmed region names. The picker passes
// a window's exact rectangle through unchanged, so the match is a near-exact
// rectangle comparison; a freehand draw matches nothing.
const App::PickableWindow* App::matchPickedWindow(uint32_t displayId, const RegionOfInterest& region) const
{
    const PickableWindow* best = nullptr;
    double bestDelta = 0.0;
    for (const PickableWindow& candidate : m_pickableWindows) {
        if (candidate.displayId != displayId) {
            continue;
        }
        const double delta = std::abs(candidate.region.leftPercent - region.leftPercent) +
                             std::abs(candidate.region.topPercent - region.topPercent) +
                             std::abs(candidate.region.rightPercent - region.rightPercent) +
                             std::abs(candidate.region.bottomPercent - region.bottomPercent);
        if (best == nullptr || delta < bestDelta) {
            best = &candidate;
            bestDelta = delta;
        }
    }
    // One percent of summed edge error tolerates the picker's coordinate
    // round-trip while rejecting a hand-drawn rectangle.
    constexpr double MatchTolerance = 1.0;

    return (best != nullptr && bestDelta <= MatchTolerance) ? best : nullptr;
}

void App::dumpSuggestionsIfRequested(const std::vector<PickerDisplay>& pickerDisplays)
{
    // Field diagnosis: dump exactly what the pipeline saw. Enable with
    // `launchctl setenv SIDESCOPES_DEBUG_SUGGESTIONS 1`.
    if (!debugSuggestionsRequested()) {
        return;
    }
    std::FILE* report = openDebugFile("/tmp/sidescopes-suggestions.txt", "w");
    if (report) {
        for (const auto& entry : pickerDisplays) {
            for (const auto& suggestion : entry.windows) {
                std::fprintf(report, "display %u suggestion '%s' %.1f,%.1f..%.1f,%.1f%%\n", entry.displayId,
                             suggestion.label.c_str(), suggestion.region.leftPercent, suggestion.region.topPercent,
                             suggestion.region.rightPercent, suggestion.region.bottomPercent);
            }
        }
        std::fclose(report);
    }
    // The dump is skipped when no frame is held.
    (void)m_worker.withLatestFrame([&](const FrameView& view) {
        std::FILE* image = openDebugFile("/tmp/sidescopes-frame.ppm", "wb");
        if (!image) {
            return;
        }
        std::fprintf(image, "P6\n%d %d\n255\n", view.width / 2, view.height / 2);
        for (int py = 0; py < view.height - 1; py += 2) {
            for (int px = 0; px < view.width - 1; px += 2) {
                const Color color = view.colorAt(px, py);
                std::fputc(color.r, image);
                std::fputc(color.g, image);
                std::fputc(color.b, image);
            }
        }
        std::fclose(image);
    });
}

void App::handleRegionBorderEdit()
{
    // The region border is live: dragging its edges, corners, or move tab
    // adjusts the region it currently outlines - the attached region of the
    // focused tracked window, or the global one - with the scopes following.
    if (m_regionPicking) {
        m_attachBorderEditing = false;

        return;
    }
    const RegionBorderEdit edit = pollRegionBorderEdit();
    if (edit.editing && !m_attachBorderEditing) {
        // Latch what the border showed when the drag began: no focus race
        // can reroute the edit to the other region kind.
        m_attachBorderEditTarget = m_attachActiveWatched;
    }
    // While an attached border is dragged, a click-through veil dims
    // everything outside its window - the resize limit made visible.
    if (edit.editing && m_attachBorderEditTarget != 0 && m_attachBorderEditTarget == m_attachActiveWatched) {
        const auto windowGeom = windowGeometry(m_attachActiveWatched);
        const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay());
        if (windowGeom && geometry) {
            showAttachedEditDim(m_captureController->capturedDisplay(), displayPercentRect(*windowGeom, *geometry));
        }
    } else if (!edit.editing && m_attachBorderEditing) {
        hideAttachedEditDim();
    }
    m_attachBorderEditing = edit.editing;
    if (edit.dismissed) {
        dismissEditedBorder();
    } else if (edit.region) {
        applyBorderEdit(*edit.region);
    }
}

// The border's close affordances dismiss the region it outlines: the
// attached one stops tracking its window only, the global one resets to
// full screen; tracked windows keep their regions either way.
void App::dismissEditedBorder()
{
    if (m_attachActiveWatched != 0) {
        m_attach.remove(m_attachActiveWatched);
        unwatchWindowMotion();
        m_attachActiveWatched = 0;
        setRegion(m_globalRegion);
    } else {
        m_globalRegion = RegionOfInterest{};
        setRegion(m_globalRegion);
    }
    m_lastActivity = glfwGetTime();
}

// Routes a border drag to the region kind it began on. An edit that began
// on an attached border may NEVER fall through to the global region - no
// accidental conversion; if the attached routing cannot resolve, the edit
// is dropped instead.
void App::applyBorderEdit(const RegionOfInterest& edited)
{
    RegionOfInterest applied = edited;
    if (m_attachBorderEditTarget != 0) {
        if (m_attachBorderEditTarget != m_attachActiveWatched) {
            return;
        }
        const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay());
        const auto windowGeom = windowGeometry(m_attachActiveWatched);
        if (!geometry || !windowGeom) {
            return;
        }
        // Attached: re-derive the window-relative fraction so the region
        // keeps following its window.
        applied = m_attach.editRegion(
            edited, AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height},
            AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints});
    } else {
        m_globalRegion = edited;
    }
    m_analysis.region = applied;
    // The analysis-dirty path syncs the border this same iteration.
    m_analysisDirty = true;
    m_lastActivity = glfwGetTime();
}

void App::pollActiveRegionPick()
{
    // While the picker is up, whatever the user indicates previews on the scopes
    // immediately; confirmation keeps it, Esc restores.
    if (!m_regionPicking) {
        return;
    }
    const RegionPickPoll poll = pollRegionPick();
    if (poll.pinMode) {
        pollPinPick(poll);
    } else {
        pollRegionPreview(poll);
    }
}

void App::pollPinPick(const RegionPickPoll& poll)
{
    // Color pinning never touches the region: clicks and drags deliver areas to
    // average, and a finish just puts things back.
    std::optional<FloatColor> chip;
    if (const auto cursor = globalCursorPosition()) {
        if (displayAtPoint(*cursor).value_or(0) == m_captureController->capturedDisplay() &&
            !m_captureController->dead() && m_frameSize) {
            if (const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay())) {
                // The chip previews exactly what a click will pin: the same point
                // sample the live cursor readout takes, not an averaged patch.
                const int pixelX =
                    static_cast<int>((cursor->x - geometry->originX) * m_frameSize->width / geometry->widthPoints);
                const int pixelY =
                    static_cast<int>((cursor->y - geometry->originY) * m_frameSize->height / geometry->heightPoints);
                chip = m_worker.sampleFrameColor(pixelX, pixelY);
            }
        } else {
            // Another display: the throttled one-shot sampler already tracks the
            // cursor there.
            std::lock_guard lock(m_screenSample->mutex);
            chip = m_screenSample->color;
        }
    }
    setRegionPickChipColor(chip);

    if (poll.pinnedPoint || poll.pinnedArea) {
        applyPinnedColor(poll);
    }
    if (poll.finished || !poll.active) {
        m_regionPicking = false;
        m_regionPickSwallowCancel = false;
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
}

void App::applyPinnedColor(const RegionPickPoll& poll)
{
    std::optional<FloatColor> pinned;
    if (poll.displayId == m_captureController->capturedDisplay() && !m_captureController->dead()) {
        if (poll.pinnedPoint && m_frameSize) {
            // A plain pin samples the frame exactly like the live readout; only a
            // dragged area averages, the explicit way to ask for a swatch.
            const int pixelX = static_cast<int>(poll.pinnedPoint->xPercent / 100.0 * m_frameSize->width);
            const int pixelY = static_cast<int>(poll.pinnedPoint->yPercent / 100.0 * m_frameSize->height);
            pinned = m_worker.sampleFrameColor(pixelX, pixelY);
        } else if (poll.pinnedArea) {
            pinned = averageFrameArea(*poll.pinnedArea);
        }
    } else {
        std::lock_guard lock(m_screenSample->mutex);
        pinned = m_screenSample->color;
    }
    if (pinned) {
        m_pins.pin(*pinned);
    }
    // The click's own Shift decided: pin-and-continue stays, a plain pin ends
    // the errand.
    if (!poll.pinnedKeepOpen) {
        cancelRegionPick();
    }
    m_lastActivity = glfwGetTime();
}

void App::pollRegionPreview(const RegionPickPoll& poll)
{
    const auto applyRegion = [&](const RegionOfInterest& region) {
        if (region.leftPercent == m_analysis.region.leftPercent && region.topPercent == m_analysis.region.topPercent &&
            region.rightPercent == m_analysis.region.rightPercent &&
            region.bottomPercent == m_analysis.region.bottomPercent) {
            return;
        }
        m_analysis.region = region;
        m_analysisDirty = true;
        m_lastActivity = glfwGetTime();
    };
    // Live preview only for the tracked display: previewing a suggestion on
    // another display would flap the capture stream on every hover.
    if (poll.preview && poll.displayId == m_captureController->capturedDisplay()) {
        applyRegion(*poll.preview);
    }
    if (poll.finished || !poll.active) {
        m_regionPicking = false;
        if (poll.confirmed) {
            if (poll.displayId != 0 && poll.displayId != m_captureController->capturedDisplay()) {
                m_captureController->requestDisplay(poll.displayId);
                m_captureController->start();
            }
            confirmPickedRegion(poll);
        } else if (!m_regionPickSwallowCancel) {
            // Cancelled with Esc: detach every tracked window and reset all
            // drawing to full screen. A cancel ordered by a tool switch is
            // not the user's Esc and resets nothing.
            resetRegionToFull();
        }
        m_attachedDrawTarget.reset();
        m_regionPickSwallowCancel = false;
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
}

// The shared tail of both attached creations: the global region retires
// (one region type at a time), the motion state starts fresh, the watch
// rebinds on the next follow step, and the picked window comes up so the
// border never wraps someone else's pixels.
void App::adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region)
{
    m_globalRegion = RegionOfInterest{};
    m_attachedWindowMoving = false;
    m_attachGripActive = false;
    m_attachRegionMovedAt = -1.0;
    unwatchWindowMotion();
    m_attachActiveWatched = 0;
    raiseWindow(identity, ownerPid);
    setRegion(region);
}

// A confirmed region that names a window tracks it (or re-picks a tracked
// one) as an attached region; the constrained draw binds to its target the
// same way; a freehand draw sets the single global region.
void App::confirmPickedRegion(const RegionPickPoll& poll)
{
    const RegionOfInterest confirmed = *poll.confirmed;
    const PickableWindow* picked = matchPickedWindow(poll.displayId, confirmed);
    const auto geometry = geometryOfDisplay(poll.displayId);
    const auto display = geometry
                             ? std::optional<AttachDisplayRect>(AttachDisplayRect{
                                   geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints})
                             : std::nullopt;
    if (picked != nullptr && display) {
        adoptAttachedPick(picked->identity, picked->ownerPid,
                          m_attach.attach(picked->identity, picked->ownerPid, picked->application, picked->windowRect,
                                          *display, confirmed));

        return;
    }
    if (m_attachedDrawTarget && display) {
        const auto windowGeom = windowGeometry(m_attachedDrawTarget->identity);
        const DesktopPoint targetCentre{windowGeom ? windowGeom->x + windowGeom->width / 2.0 : 0.0,
                                        windowGeom ? windowGeom->y + windowGeom->height / 2.0 : 0.0};
        // The drawn rectangle must live on the target window's own display,
        // or the mapping is meaningless.
        if (windowGeom && !windowGeom->minimized && displayAtPoint(targetCentre).value_or(0) == poll.displayId) {
            adoptAttachedPick(
                m_attachedDrawTarget->identity, m_attachedDrawTarget->ownerPid,
                m_attach.attach(m_attachedDrawTarget->identity, m_attachedDrawTarget->ownerPid,
                                m_attachedDrawTarget->application,
                                AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height},
                                *display, confirmed));

            return;
        }
    }
    // One region type at a time: a global draw retires every attached
    // region.
    if (m_attach.attached()) {
        m_attach.detachAll();
        unwatchWindowMotion();
        m_attachActiveWatched = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
    m_globalRegion = confirmed;
    setRegion(m_globalRegion);
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
