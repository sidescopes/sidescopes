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
#include "app/row_layout.h"
#include "app/scope_layout.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
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
    MenuAttachFace,
    MenuZoom1,
    MenuZoom2,
    MenuZoom4,
    MenuAttachWindow = 30,
    MenuFullScreen,
    MenuDetachWindow,
    MenuDetachAll,
    MenuToggleGraticule = 40,
    MenuClearPinnedMarkers,
    MenuPinColor,
    MenuToggleCaptureVisibility,
    MenuToggleDiagRecording,
    MenuShowDiagLog,
    MenuResetDiagnostics,
    MenuOpenSettings = 50,
    MenuAbout,
    MenuQuit,
    MenuLayoutAuto = 60,
    MenuLayoutVertical,
    MenuLayoutHorizontal,
    // Preset load ids are MenuLoadPresetBase + slot (1-9); save ids are
    // MenuSavePresetBase + slot. Both ranges stay clear of ParamMenuActionBase.
    MenuLoadPresetBase = 70,
    MenuSavePresetBase = 80,
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

// The widest a tooltip may run before wrapping, in 100%-scale points.
inline constexpr float TooltipWrapWidth = 260.0f;

// A tooltip that wraps rather than running off the edge. Without multi-viewport
// support a tooltip cannot spill past the application window, and this one is
// often deliberately narrow - so a long line would simply be cut. Wrapping
// tracks the window when that is the tighter of the two.
void wrappedTooltip(const char* text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) || !ImGui::BeginTooltip()) {
        return;
    }
    const float margin = 4.0f * ImGui::GetStyle().WindowPadding.x;
    ImGui::PushTextWrapPos(std::min(ImGui::GetMainViewport()->Size.x - margin, TooltipWrapWidth));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
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

/// The outcome of scanning one non-streamed display: its detector boxes, the
/// grabbed frame's size (zero when the grab failed), and how long grab plus
/// detect took, for the diagnostics line.
struct ScanResult
{
    std::vector<IntRect> faces;
    int width = 0;
    int height = 0;
    double elapsedMs = 0.0;
};

// Grabs one display off the capture stream and runs the face detector on it.
// Pure of application state: it takes only the display and its point width
// (for the detector's density floor), so it is safe to run on a detached
// thread with nothing but value captures.
ScanResult scanDisplayForFaces(uint32_t displayId, double widthPoints)
{
    const auto started = std::chrono::steady_clock::now();
    ScanResult result;
    if (const std::optional<CapturedImage> image = captureDisplayImage(displayId)) {
        FrameView view;
        view.bgra = image->bgra.data();
        view.strideBytes = image->width * 4;
        view.width = image->width;
        view.height = image->height;
        const float pixelsPerPoint = widthPoints > 0.0 ? static_cast<float>(image->width / widthPoints) : 1.0f;
        result.faces = detectFaces(view, pixelsPerPoint);
        result.width = image->width;
        result.height = image->height;
    }
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - started;
    result.elapsedMs = elapsed.count();

    return result;
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
    // ImGui's default range stops at U+00FF, which would drop the delta the
    // color picker labels its differences with. Latin-1 plus that one glyph.
    static constexpr ImWchar InterfaceGlyphRanges[] = {0x0020, 0x00FF, 0x0394, 0x0394, 0};
    config.GlyphRanges = InterfaceGlyphRanges;
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
// windows to suggest and in what order.
std::vector<SuggestedRegion> windowSuggestionsFor(uint32_t displayId)
{
    const auto geometry = geometryOfDisplay(displayId);
    if (!geometry) {
        return {};
    }

    // The most windows the picker suggests at once, so the overlay stays
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
// copies the hex; the pinned colors (P) ride along as small
// swatches with the same click.
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

// The monospace face may sit at a size of its own (monospaceFontScale),
// and ImGui top-aligns the items sharing a line, so a value drawn beside
// an interface-font label lands on a different baseline. Sinking the
// value by the ascent difference seats both on the label's baseline; the
// line keeps its own top, so only this item shifts. Rows that position
// their text by hand use pushHexFont directly instead.
float hexFontBaselineDrop(ImFont* font)
{
    const float interfaceAscent = ImGui::GetFontBaked()->Ascent;
    pushHexFont(font);
    const float drop = interfaceAscent - ImGui::GetFontBaked()->Ascent;
    popHexFont(font);

    return drop;
}

void hexFontText(ImFont* font, const char* text)
{
    const float drop = hexFontBaselineDrop(font);
    pushHexFont(font);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + drop);
    ImGui::TextUnformatted(text);
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
// fixed-width font, everything else draws in the interface font. AddText
// is top-aligned, so a passed font sinks by the ascent difference and
// shares the interface font's baseline.
void swatchText(ImDrawList* draw, const ImVec2& pos, ImU32 ink, ImU32 shadow, const char* text, ImFont* font = nullptr)
{
    const ImVec2 seated(pos.x, pos.y + (font ? hexFontBaselineDrop(font) : 0.0f));
    if (font) {
        ImGui::PushFont(font, font->LegacySize);
    }
    draw->AddText(ImVec2(seated.x, seated.y + 1.0f), shadow, text);
    draw->AddText(seated, ink, text);
    if (font) {
        ImGui::PopFont();
    }
}

// Tooltip strings shared by the deck header, its rows, and the difference row
// under the hero. One line per column: a paragraph covering all three tells a
// reader about two quantities they did not ask about. The sign gloss is the
// convention every colorimetric tool follows; hue has none, because it runs
// around a circle rather than along an axis.
constexpr const char* PickerDeltaETip = "CIEDE2000 difference from the live color, lower is closer (sRGB assumed)";
constexpr const char* PickerLchTips[3] = {
    "how much lighter (+) or darker (-) the live color is",
    "how much more colorful (+) or duller (-) the live color is",
    "how far the live color's hue has drifted - counts for less when the color is dull",
};
constexpr const char* PickerRgbTips[3] = {
    "how much more (+) or less (-) red the live color is",
    "how much more (+) or less (-) green the live color is",
    "how much more (+) or less (-) blue the live color is",
};

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
    bool showDeltaE;
    bool showLch;
    bool showRgb;
    float deltaERight;
    float lchRight[3];
    float rgbRight[3];
    float deltaETypical;
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
        char pinnedTip[48];
        std::snprintf(pinnedTip, sizeof(pinnedTip), "pinned  %s - click to copy", pinHex);
        wrappedTooltip(pinnedTip);
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

// The CIEDE2000 distance itself, one decimal the way the field quotes it. It
// replaced a friendlier "match percentage": that read as a claim about how
// alike two colors LOOK, which the measure cannot honor at the distances this
// picker works over - it is built and validated for small differences.
void formatDeltaE(float deltaE, char (&value)[8])
{
    std::snprintf(value, sizeof(value), "%.1f", deltaE);
}

// The difference triplet's own metrics. Nothing below the hero lines up with
// it any more - the channel percentages that used to sit above it live in the
// status bar now - so a group is only as wide as it reads: the value sits one
// gap from its label, and the space between groups does the separating.
struct DiffColumns
{
    float label;
    float stride;
    float width;
};

DiffColumns measureDiffColumns(const PickerContext& ctx)
{
    DiffColumns columns{};
    columns.label = ImGui::CalcTextSize("ΔL").x;
    const float value = ImGui::CalcTextSize("-199").x;
    const float group = columns.label + ctx.columnGap + value;
    columns.stride = group + 3.0f * ctx.columnGap;
    columns.width = 2.0f * columns.stride + group;

    return columns;
}

// Three groups - lightness, chroma, hue - each a label and the signed number it
// names, held together by sitting closer to each other than to their neighbors.
void drawPickerDiffTriplet(const PickerContext& ctx, float valuesStart, const float diffValues[3],
                           const DiffColumns& columns)
{
    const char* diffLabels[3] = {"ΔL", "ΔC", "ΔH"};
    for (int component = 0; component < 3; ++component) {
        const float columnStart = valuesStart + static_cast<float>(component) * columns.stride;
        if (component == 0) {
            ImGui::SetCursorPosX(columnStart);
        } else {
            ImGui::SameLine(columnStart);
        }
        ImGui::TextUnformatted(diffLabels[component]);
        wrappedTooltip(PickerLchTips[component]);
        char value[8];
        std::snprintf(value, sizeof(value), "%+d", static_cast<int>(std::lround(diffValues[component])));
        ImGui::SameLine(columnStart + columns.label + ctx.columnGap);
        ImGui::TextUnformatted(value);
        wrappedTooltip(PickerLchTips[component]);
    }
}

// Everything under the hero: where the live color sits relative to the pinned
// one, and how far apart they are overall. The triplet leads and the distance
// closes the line; when the pane cannot seat both, the distance drops to its
// own line rather than pushing the detail off the pane. The live color's hex is not
// here - it changes with every mouse move and cannot be copied, and the deck
// carries the hexes that are worth keeping.
void drawPickerDifferenceRow(const PickerContext& ctx, const ImVec2& area, float valuesStart)
{
    const ColorDifference difference = differenceFrom(labFromSrgb(ctx.pins.comparatorColor()), labFromSrgb(ctx.color));
    char deltaEValue[8];
    formatDeltaE(difference.deltaE, deltaEValue);
    const float deltaEValueX = area.x - hexFontWidth(ctx.monospaceFont, deltaEValue);
    const float deltaELabelX = deltaEValueX - ctx.columnGap - ImGui::CalcTextSize("ΔE").x;
    const float diffValues[3] = {difference.lightness, difference.chroma, difference.hue};
    const DiffColumns columns = measureDiffColumns(ctx);
    const bool tripletShares = valuesStart + columns.width + ctx.columnGap <= deltaELabelX;
    if (tripletShares || valuesStart + columns.width <= area.x) {
        drawPickerDiffTriplet(ctx, valuesStart, diffValues, columns);
    }
    // Right-aligned while it closes the triplet's line; flush left once it has
    // a line of its own, the way the region toolbox wraps.
    float labelX = deltaELabelX;
    float valueX = deltaEValueX;
    if (tripletShares) {
        ImGui::SameLine(labelX);
    } else {
        labelX = valuesStart;
        valueX = labelX + ImGui::CalcTextSize("ΔE").x + ctx.columnGap;
        ImGui::SetCursorPosX(labelX);
    }
    ImGui::TextUnformatted("ΔE");
    wrappedTooltip(PickerDeltaETip);
    ImGui::SameLine(valueX);
    hexFontText(ctx.monospaceFont, deltaEValue);
    wrappedTooltip(PickerDeltaETip);
}

// Progressive disclosure: the distance first, then L/C/H, then R/G/B, each group
// admitted only if the whole block still clears the hex column.
void admitDeckGroups(float leftPartEnd, float deckWidth, float blockGap, float columnGap, float deltaECol,
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
    layout.showDeltaE = admitGroup(deltaECol);
    layout.showLch = layout.showDeltaE && admitGroup(lchGroupWidth);
    layout.showRgb = layout.showLch && admitGroup(rgbGroupWidth);
}

// Right edges of every visible column, walked left to right from the block's
// left edge. The block anchors just past the hex column, not the far edge.
void computeDeckRights(float leftPartEnd, float blockGap, float columnGap, float deltaECol, const float lchCol[3],
                       const float rgbCol[3], DeckLayout& layout)
{
    float walk = leftPartEnd + blockGap;
    if (layout.showDeltaE) {
        layout.deltaERight = walk + deltaECol;
        walk = layout.deltaERight;
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
    const float deltaECol = std::max(hexFontWidth(ctx.monospaceFont, "199.9"), ImGui::CalcTextSize("ΔE").x);
    // Every numeric column in the deck is a difference against the live color,
    // so each carries the delta the hero row's absolute channels do without.
    const char* lchLabels[3] = {"ΔL", "ΔC", "ΔH"};
    const char* rgbLabels[3] = {"ΔR", "ΔG", "ΔB"};
    float lchCol[3];
    float rgbCol[3];
    for (int column = 0; column < 3; ++column) {
        lchCol[column] = std::max(hexFontWidth(ctx.monospaceFont, "+199"), ImGui::CalcTextSize(lchLabels[column]).x);
        rgbCol[column] = std::max(hexFontWidth(ctx.monospaceFont, "+100%"), ImGui::CalcTextSize(rgbLabels[column]).x);
    }
    const float lchGroupWidth = lchCol[0] + lchCol[1] + lchCol[2] + 2.0f * (2.0f * ctx.columnGap);
    const float rgbGroupWidth = rgbCol[0] + rgbCol[1] + rgbCol[2] + 2.0f * (2.0f * ctx.columnGap);
    const float blockGap = 3.0f * ctx.columnGap;
    admitDeckGroups(leftPartEnd, deckWidth, blockGap, ctx.columnGap, deltaECol, lchGroupWidth, rgbGroupWidth, layout);
    computeDeckRights(leftPartEnd, blockGap, ctx.columnGap, deltaECol, lchCol, rgbCol, layout);
    // Each header label centers over the ink of a typical value - a sign and two
    // digits, right-aligned - rather than over the column box.
    layout.lchTypical = hexFontWidth(ctx.monospaceFont, "+34");
    layout.rgbTypical = hexFontWidth(ctx.monospaceFont, "+34%");
    layout.deltaETypical = hexFontWidth(ctx.monospaceFont, "77.7");

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
        ImGui::TextUnformatted(label);
        wrappedTooltip(tip);
    };
    if (layout.showDeltaE) {
        headerCell(layout.deltaERight, layout.deltaETypical, "ΔE", PickerDeltaETip);
    }
    if (layout.showLch) {
        headerCell(layout.lchRight[0], layout.lchTypical, "ΔL", PickerLchTips[0]);
        headerCell(layout.lchRight[1], layout.lchTypical, "ΔC", PickerLchTips[1]);
        headerCell(layout.lchRight[2], layout.lchTypical, "ΔH", PickerLchTips[2]);
    }
    if (layout.showRgb) {
        headerCell(layout.rgbRight[0], layout.rgbTypical, "ΔR", PickerRgbTips[0]);
        headerCell(layout.rgbRight[1], layout.rgbTypical, "ΔG", PickerRgbTips[1]);
        headerCell(layout.rgbRight[2], layout.rgbTypical, "ΔB", PickerRgbTips[2]);
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
    wrappedTooltip("remove this pin");
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
    wrappedTooltip(selected ? "click to unload from the comparator" : "click to compare against the live color");
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
    // textDrop tops the text where the row's framed neighbours top their
    // labels; the ascent difference then seats the fixed-width text on
    // the same baseline as the cross button beside it.
    const float seat = textDrop + hexFontBaselineDrop(ctx.monospaceFont);
    char pinHex[8];
    pinHexOf(ctx.pins, index, pinHex);
    ImGui::SameLine(layout.hexX);
    ImGui::SetCursorPosY(rowPosY + seat);
    pushHexFont(ctx.monospaceFont);
    ImGui::TextUnformatted(pinHex);
    popHexFont(ctx.monospaceFont);
    if (ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(pinHex);
    }
    wrappedTooltip("click to copy");
    const ColorDifference pinDiff = differenceFrom(labFromSrgb(ctx.pins.color(index)), labFromSrgb(ctx.color));
    const auto numericCell = [&](float colRight, const char* value, const char* tip) {
        ImGui::SameLine(colRight - hexFontWidth(ctx.monospaceFont, value));
        ImGui::SetCursorPosY(rowPosY + seat);
        pushHexFont(ctx.monospaceFont);
        ImGui::TextUnformatted(value);
        popHexFont(ctx.monospaceFont);
        wrappedTooltip(tip);
    };
    if (layout.showDeltaE) {
        char deltaEValue[8];
        formatDeltaE(pinDiff.deltaE, deltaEValue);
        numericCell(layout.deltaERight, deltaEValue, PickerDeltaETip);
    }
    if (layout.showLch) {
        const float lchValues[3] = {pinDiff.lightness, pinDiff.chroma, pinDiff.hue};
        for (int column = 0; column < 3; ++column) {
            char value[8];
            std::snprintf(value, sizeof(value), "%+d", static_cast<int>(std::lround(lchValues[column])));
            numericCell(layout.lchRight[column], value, PickerLchTips[column]);
        }
    }
    if (layout.showRgb) {
        const float pinChannels[3] = {ctx.pins.color(index).r, ctx.pins.color(index).g, ctx.pins.color(index).b};
        const float liveChannels[3] = {ctx.color.r, ctx.color.g, ctx.color.b};
        for (int column = 0; column < 3; ++column) {
            char value[8];
            std::snprintf(value, sizeof(value), "%+.0f%%", (liveChannels[column] - pinChannels[column]) / 2.55f);
            numericCell(layout.rgbRight[column], value, PickerRgbTips[column]);
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
        char deckTip[64];
        std::snprintf(deckTip, sizeof(deckTip), "%s - click to compare, right-click to manage", pinHex);
        wrappedTooltip(deckTip);
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
// or the hex line copies the hex; the pinned colors (P) ride along.
// Three size tiers, few and spaced so resizing feels like deliberate steps: a
// strip, a compact comparator, the full reference deck. Order never changes -
// comparator, values, pins - and only the comparator absorbs extra height.
// The column metrics of the readout a roomy hero carries on the swatch itself:
// a channel letter, its percentage, and the hex under them.
struct PickerColumns
{
    float label;
    float value;
    float gap;
    float stride;
};

// Every value owns a column sized for its widest form and right-aligns inside
// it - the cure for layouts that twitch as digits come and go. The difference
// row below the hero measures itself (see measureDiffColumns): it carries
// wider labels and signed readings, and nothing lines the two up.
PickerColumns measurePickerColumns()
{
    PickerColumns columns{};
    columns.label = ImGui::CalcTextSize("R").x;
    columns.value = ImGui::CalcTextSize("100%").x;
    columns.gap = ImGui::CalcTextSize(" ").x;
    columns.stride = columns.label + columns.gap + columns.value + 2.0f * columns.gap;

    return columns;
}

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
    const PickerColumns columns = measurePickerColumns();
    const float labelColumn = columns.label;
    const float percentColumn = columns.value;
    const float columnGap = columns.gap;
    const float channelStride = columns.stride;
    const float hexWidth = hexFontWidth(monospaceFont, hex);
    const PickerContext ctx{color,     pins,          monospaceFont, labelColumn, percentColumn,
                            columnGap, channelStride, hexWidth,      lineHeight,  hex};

    const PickerHero hero = computePickerHero(ctx, area, style);
    drawPickerHero(ctx, hero);
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

// The lowercase orientation word for a menu summary line.
const char* orientationName(LayoutOrientation orientation)
{
    switch (orientation) {
    case LayoutOrientation::Vertical:
        return "vertical";
    case LayoutOrientation::Horizontal:
        return "horizontal";
    case LayoutOrientation::Automatic:
    default:
        return "auto";
    }
}

// The Presets submenu entry text: "N - empty" for an unused slot, otherwise a
// short summary like "1 - VWH" from the saved stack tokens, naming the
// orientation only when the preset pins one - Automatic is the unspoken
// default.
std::string presetLabel(int slot, const LayoutPreset& preset)
{
    const std::string number = std::to_string(slot);
    if (preset.stack.empty()) {
        return number + " - empty";
    }
    std::string label = number + " - " + preset.stack;
    const LayoutOrientation orientation = orientationFromInt(preset.orientation);
    if (orientation != LayoutOrientation::Automatic) {
        label += ' ';
        label += orientationName(orientation);
    }

    return label;
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

/// The face-lock probe cadence. The lock adopts only positions two
/// consecutive probes agree on, so this bounds how long the settle snap
/// trails the end of a pan or zoom; the ROI stays small enough that the
/// reference laptop shrugs it off.
constexpr double FaceLockProbeSeconds = 0.3;

/// The probe's reach around the last adopted anchor, in anchor widths:
/// covers the decision gates' proximity radius plus the face's own extent.
constexpr double FaceLockRoiWidths = 2.5;

/// The content-stability probe: mean absolute per-byte difference of the
/// region's sample grid that counts as "the content changed", and how long
/// the content must sit still before the border may show again. The
/// threshold sits just above capture noise: a pan over smooth skin moves
/// pixels only a little, and the border must hide on it instantly too.
constexpr double ContentChangeThreshold = 2.5;
constexpr double ContentSettleSeconds = 0.45;

bool sameRegionRect(const RegionOfInterest& a, const RegionOfInterest& b)
{
    return std::abs(a.leftPercent - b.leftPercent) < 0.01 && std::abs(a.topPercent - b.topPercent) < 0.01 &&
           std::abs(a.rightPercent - b.rightPercent) < 0.01 && std::abs(a.bottomPercent - b.bottomPercent) < 0.01;
}

// Maps a display-percent region onto a frame's pixel grid, where the face
// lock does its geometry, and back.
LockRect lockRectFromPercent(const RegionOfInterest& region, int frameWidth, int frameHeight)
{
    return LockRect{region.leftPercent / 100.0 * frameWidth, region.topPercent / 100.0 * frameHeight,
                    region.rightPercent / 100.0 * frameWidth, region.bottomPercent / 100.0 * frameHeight};
}

RegionOfInterest percentFromLockRect(const LockRect& rect, int frameWidth, int frameHeight)
{
    return RegionOfInterest{rect.left * 100.0 / frameWidth, rect.top * 100.0 / frameHeight,
                            rect.right * 100.0 / frameWidth, rect.bottom * 100.0 / frameHeight};
}

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

    observeSystemWake([this] { m_captureController->markStale(); });
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
    if (m_captureController->capturedDisplay() == 0) {
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
        if (!attached && m_captureController->capturedDisplay() != m_displayLabelId) {
            m_displayLabelId = m_captureController->capturedDisplay();
            m_displayLabel = borderLabelFrom(displayName(m_displayLabelId), "Display");
        }
        showRegionBorder(m_captureController->capturedDisplay(), m_analysis.region,
                         attached ? m_attachActiveLabel : m_displayLabel, attached);
    }
}

std::optional<FloatColor> App::averageFrameColor(const RegionOfInterest& region) const
{
    // Averages a display-percent region of the latest frame: a dragged pin's
    // sample. A drag is the explicit request to average textured pixels; a
    // plain click samples a point instead, matching the live readout.
    std::optional<FloatColor> color;
    const bool sampled = m_worker.withLatestFrame([&](const FrameView& view) {
        const int left = std::clamp(static_cast<int>(region.leftPercent / 100.0 * view.width), 0, view.width);
        const int right = std::clamp(static_cast<int>(region.rightPercent / 100.0 * view.width), 0, view.width);
        const int top = std::clamp(static_cast<int>(region.topPercent / 100.0 * view.height), 0, view.height);
        const int bottom = std::clamp(static_cast<int>(region.bottomPercent / 100.0 * view.height), 0, view.height);
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

// Gathers this frame's observation for every attached window: geometry,
// minimized state, and - for visible ones - the display it sits on.
std::vector<AttachedWindowObservation> App::gatherAttachedObservations() const
{
    std::vector<AttachedWindowObservation> observations;
    for (const uint64_t identity : m_attach.attachedIdentities()) {
        AttachedWindowObservation observation;
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
    if (decision.activeIdentity == 0 || decision.activeIdentity != m_activeWindowIdentity) {
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
// the focused attached window's region when there is one, the global region
// otherwise. Motion detection is the follow step's business - a region
// change here may equally be the active window switching, which must not
// blank the border.
void App::applyAttachDecision(const AttachDecision& decision)
{
    setRegion(decision.region ? *decision.region : m_globalRegion);
    if (decision.closedCount > 0) {
        m_attachDetachNotice = decision.detachedAll ? "window closed - detached" : "window closed - still attached";
        m_attachNoticeUntil = glfwGetTime() + 5.0;
    }
    if (decision.detachedAll) {
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
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
        // A click into an attached window is often a focus change too: wake
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

// The label prefers the window's live title - the filename in most editors
// - and follows it when the window's content changes; a face-locked window
// says so.
void App::refreshAttachedLabel(const AttachDecision& decision)
{
    if (decision.activeIdentity == 0) {
        return;
    }
    m_attachActiveLabel = borderLabelFrom(decision.activeTitle, m_attach.activeApplicationName());
    if (m_faceLocks.contains(decision.activeIdentity)) {
        m_attachActiveLabel = "face - " + m_attachActiveLabel;
    }
}

// The focused window drives everything: the foreground application's
// frontmost ordinary window - frozen on the active window while its border
// is being dragged, and held while SideScopes itself is in front or no
// application is. One region kind at a time means there is no global region
// to switch to while windows are attached, so the user can work the scopes
// against the last attached region without losing it.
std::optional<uint64_t> App::resolveFocusedWindow() const
{
    if (m_attachBorderEditing && m_activeWindowIdentity != 0) {
        return m_activeWindowIdentity;
    }
    const int64_t foreground = foregroundApplicationPid();
    const uint64_t held = m_activeWindowIdentity != 0 ? m_activeWindowIdentity : m_attach.activeIdentity();
    if ((foreground == m_ownPid || foreground == 0) && held != 0) {
        // Straight after a pick the held window is the picked one - the
        // watch has not bound yet, and a window owned by a helper process
        // (a Quick Look preview) can never take the foreground for itself,
        // so this is the only thing keeping its region. A foreground of
        // zero is a focus handoff in flight - Windows reports no foreground
        // window for a frame mid-click - and must hold too: rerouting on
        // that frame wipes the held identity, so the region could never
        // survive a click into SideScopes itself.
        return held;
    }

    return focusedAttachedWindow(foreground, m_attach.attachedIdentities());
}

// One follow step: observes every attached window, lets the controller pick
// the active one and map its region, and applies the verdict. Runs twice per
// frame - once before the frame, and again right after the swap so the
// border and region are repositioned from geometry read after the vsync
// wait, not a frame earlier.
void App::followAttachedWindow()
{
    if (!m_attach.attached() || m_regionPicking) {
        return;
    }

    const AttachDecision decision = m_attach.observe(gatherAttachedObservations(), resolveFocusedWindow());
    if (activeWindowMoved(decision)) {
        onWindowMotion(WindowMotionSignal::Moved);
    }
    refreshAttachedLabel(decision);
    if (decision.activeIdentity != m_activeWindowIdentity) {
        // The active window switched: the motion watch moves with it and the
        // border, no longer mid-anything, follows the routing right away.
        m_activeWindowIdentity = decision.activeIdentity;
        m_attachGripActive = false;
        m_attachedWindowMoving = false;
        unwatchWindowMotion();
        if (m_activeWindowIdentity != 0) {
            watchWindowMotion(m_activeWindowIdentity, decision.activeOwnerPid,
                              [this](WindowMotionSignal signal) { onWindowMotion(signal); });
        }
        // A face lock's anchor goes stale across a focus gap: dressing the
        // border from it flashes a wrong region for one probe's latency -
        // invisible on a fast detector, half a second on a slow one. Hold
        // the border until this activation's first verdict, probing now
        // rather than waiting out the cadence. A recently verified anchor
        // (a fresh pick, a quick focus flip) keeps its instant border.
        const auto activated = m_faceLocks.find(m_activeWindowIdentity);
        if (activated != m_faceLocks.end() &&
            glfwGetTime() - activated->second.anchorVerifiedAt > FaceLockProbeSeconds) {
            m_faceLockHunting = true;
            m_nextFaceLockProbe = 0.0;
        }
        m_lastActivity = glfwGetTime();
    }
    m_attachLastSeenRect = decision.activeRect;
    applyAttachDecision(decision);
    captureActiveDisplay(decision);
    SS_DIAG(Attach, "fg=%lld active=%llu display=%u region=%.1f,%.1f,%.1f,%.1f label=%s moving=%d",
            static_cast<long long>(foregroundApplicationPid()),
            static_cast<unsigned long long>(decision.activeIdentity), m_captureController->capturedDisplay(),
            m_analysis.region.leftPercent, m_analysis.region.topPercent, m_analysis.region.rightPercent,
            m_analysis.region.bottomPercent, m_attachActiveLabel.c_str(), m_attachedWindowMoving ? 1 : 0);
    updateFaceLock(decision);
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

// The probe's region of interest, in frame pixels: FaceLockRoiWidths anchor
// widths around the last adopted anchor, clipped to the attached window and
// the frame - a neighbouring window's faces are structurally out of reach.
// A lock that has lost its face searches the whole attached window instead.
IntRect faceLockRoi(const FaceLockState& lock, const AttachWindowRect& window, const DisplayGeometry& geometry,
                    int frameWidth, int frameHeight)
{
    const double scale = frameWidth / geometry.widthPoints;
    double left = (window.x - geometry.originX) * scale;
    double top = (window.y - geometry.originY) * scale;
    double right = left + window.width * scale;
    double bottom = top + window.height * scale;
    if (!face_lock::searchingWide(lock)) {
        const double reach = FaceLockRoiWidths * lock.lastAnchor.width;
        left = std::max(left, lock.lastAnchor.centerX - reach);
        top = std::max(top, lock.lastAnchor.centerY - reach);
        right = std::min(right, lock.lastAnchor.centerX + reach);
        bottom = std::min(bottom, lock.lastAnchor.centerY + reach);
    }
    left = std::max(left, 0.0);
    top = std::max(top, 0.0);
    right = std::min(right, static_cast<double>(frameWidth));
    bottom = std::min(bottom, static_cast<double>(frameHeight));

    return IntRect{static_cast<int>(left), static_cast<int>(top), static_cast<int>(right - left),
                   static_cast<int>(bottom - top)};
}

// A window translation carries the face with it, so the anchors ride along
// and the probe keeps searching where the face actually is. A resize
// re-lays the content out unpredictably, so the anchors stay put and the
// probes re-find the face instead.
void App::carryLockWithWindow(AppFaceLock& lock, const AttachWindowRect& rect)
{
    if (!lock.windowRect) {
        lock.windowRect = rect;

        return;
    }
    const bool sameSize =
        std::abs(rect.width - lock.windowRect->width) < 0.5 && std::abs(rect.height - lock.windowRect->height) < 0.5;
    const double dx = rect.x - lock.windowRect->x;
    const double dy = rect.y - lock.windowRect->y;
    if (sameSize && (dx != 0.0 || dy != 0.0) && m_frameSize) {
        if (const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay())) {
            const double scale = m_frameSize->width / geometry->widthPoints;
            face_lock::translate(lock.state, dx * scale, dy * scale);
        }
    }
    lock.windowRect = rect;
}

// A cheap content-stability probe over the face-locked region: a sparse grid
// of pixels compared frame to frame. Any considerable change - a pan, a zoom,
// even a develop-slider drag - hides the border until the content settles;
// the scopes keep analyzing the region throughout. A region rectangle we
// moved ourselves only refreshes the baseline.
void App::probeRegionContentChange()
{
    constexpr int GridSide = 16;

    if (!m_frameSize) {
        return;
    }
    const RegionOfInterest region = m_analysis.region;
    std::vector<uint8_t> samples;
    samples.reserve(static_cast<std::size_t>(GridSide) * GridSide * 3);
    const bool sampled = m_worker.withLatestFrame([&](const FrameView& view) {
        const double left = region.leftPercent / 100.0 * view.width;
        const double top = region.topPercent / 100.0 * view.height;
        const double width = (region.rightPercent - region.leftPercent) / 100.0 * view.width;
        const double height = (region.bottomPercent - region.topPercent) / 100.0 * view.height;
        for (int gridY = 0; gridY < GridSide; ++gridY) {
            for (int gridX = 0; gridX < GridSide; ++gridX) {
                const int px = std::clamp(static_cast<int>(left + (gridX + 0.5) * width / GridSide), 0, view.width - 1);
                const int py =
                    std::clamp(static_cast<int>(top + (gridY + 0.5) * height / GridSide), 0, view.height - 1);
                const uint8_t* pixel = view.pixelAt(px, py);
                samples.push_back(pixel[0]);
                samples.push_back(pixel[1]);
                samples.push_back(pixel[2]);
            }
        }
    });
    if (!sampled || samples.empty()) {
        return;
    }
    if (sameRegionRect(region, m_regionContentRect) && samples.size() == m_regionContentSamples.size()) {
        long long total = 0;
        for (std::size_t index = 0; index < samples.size(); ++index) {
            total += std::abs(static_cast<int>(samples[index]) - static_cast<int>(m_regionContentSamples[index]));
        }
        if (static_cast<double>(total) / static_cast<double>(samples.size()) > ContentChangeThreshold) {
            m_regionContentChangedAt = glfwGetTime();
        }
    }
    m_regionContentRect = region;
    m_regionContentSamples = std::move(samples);
}

bool App::regionContentUnsettled() const
{
    return m_regionContentChangedAt >= 0.0 && glfwGetTime() - m_regionContentChangedAt < ContentSettleSeconds;
}

// The per-frame face-lock step: prunes locks whose windows are gone, drains
// a finished probe, and starts the next one when the active window's lock
// is due. All adopt-or-hold judgement lives in face_lock::decide; this only
// moves data between the threads.
void App::updateFaceLock(const AttachDecision& decision)
{
    std::erase_if(m_faceLocks, [this](const auto& entry) { return !m_attach.isAttached(entry.first); });
    if (m_faceLockProbe.ready.load()) {
        consumeFaceLockProbe(decision);
    }
    const auto locked = m_faceLocks.find(decision.activeIdentity);
    if (decision.activeIdentity == 0 || locked == m_faceLocks.end()) {
        m_faceLockHunting = false;
        m_regionContentChangedAt = -1.0;

        return;
    }
    if (decision.activeRect) {
        carryLockWithWindow(locked->second, *decision.activeRect);
    }
    probeRegionContentChange();
    // The probe never runs against a mid-gesture window: the user's drag
    // wins, and the lock catches up once things settle.
    if (m_attachBorderEditing || m_attachedWindowMoving || m_attachGripActive) {
        return;
    }
    const double now = glfwGetTime();
    if (m_faceLockProbe.running.load() || now < m_nextFaceLockProbe) {
        return;
    }
    m_nextFaceLockProbe = now + FaceLockProbeSeconds;
    launchFaceLockProbe(decision, locked->second.state);
}

// Copies the probe's region of interest out of the latest frame and hands
// it to a detached detection thread, so detection never hitches a frame.
void App::launchFaceLockProbe(const AttachDecision& decision, const FaceLockState& lock)
{
    const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay());
    if (!geometry || !decision.activeRect) {
        return;
    }
    auto pixels = std::make_shared<std::vector<uint8_t>>();
    IntRect roi;
    float pixelsPerPoint = 1.0f;
    const bool copied = m_worker.withLatestFrame([&](const FrameView& view) {
        roi = faceLockRoi(lock, *decision.activeRect, *geometry, view.width, view.height);
        if (roi.width <= 0 || roi.height <= 0) {
            return;
        }
        pixelsPerPoint = static_cast<float>(view.width / geometry->widthPoints);
        const std::size_t rowBytes = static_cast<std::size_t>(roi.width) * 4;
        pixels->resize(rowBytes * static_cast<std::size_t>(roi.height));
        for (int row = 0; row < roi.height; ++row) {
            std::memcpy(pixels->data() + rowBytes * static_cast<std::size_t>(row),
                        view.bgra + static_cast<std::size_t>(roi.y + row) * view.strideBytes +
                            static_cast<std::size_t>(roi.x) * 4,
                        rowBytes);
        }
    });
    if (!copied || pixels->empty()) {
        return;
    }
    m_faceLockProbe.forWindowIdentity = decision.activeIdentity;
    m_faceLockProbe.roi = roi;
    m_faceLockProbe.running.store(true);
    FaceLockProbe* probe = &m_faceLockProbe;
    std::thread([probe, pixels, roi, pixelsPerPoint] {
        FrameView view;
        view.bgra = pixels->data();
        view.strideBytes = roi.width * 4;
        view.width = roi.width;
        view.height = roi.height;
        std::vector<IntRect> faces = detectFaces(view, pixelsPerPoint);
        for (IntRect& box : faces) {
            box.x += roi.x;
            box.y += roi.y;
        }
        {
            std::lock_guard lock(probe->mutex);
            probe->faces = std::move(faces);
        }
        probe->ready.store(true);
        // The wake goes out before the running flag clears: the shutdown
        // drain waits on running, and GLFW must still be alive to hear it.
        glfwPostEmptyEvent();
        probe->running.store(false);
    }).detach();
}

// Drains the finished probe and lets the pure core judge it against the
// gates. Every verdict is logged for grading; only an adoption touches the
// region.
void App::consumeFaceLockProbe(const AttachDecision& decision)
{
    std::vector<IntRect> boxes;
    {
        std::lock_guard lock(m_faceLockProbe.mutex);
        boxes = std::move(m_faceLockProbe.faces);
        m_faceLockProbe.faces.clear();
    }
    m_faceLockProbe.ready.store(false);
    const auto locked = m_faceLocks.find(m_faceLockProbe.forWindowIdentity);
    if (locked == m_faceLocks.end() || m_faceLockProbe.forWindowIdentity != decision.activeIdentity) {
        return;
    }
    const IntRect& roi = m_faceLockProbe.roi;
    const LockRect roiRect{static_cast<double>(roi.x), static_cast<double>(roi.y),
                           static_cast<double>(roi.x + roi.width), static_cast<double>(roi.y + roi.height)};
    std::vector<FaceAnchor> candidates;
    candidates.reserve(boxes.size());
    std::size_t edgeDropped = 0;
    for (const IntRect& box : boxes) {
        const LockRect boxRect{static_cast<double>(box.x), static_cast<double>(box.y),
                               static_cast<double>(box.x + box.width), static_cast<double>(box.y + box.height)};
        if (!face_lock::trustworthyBox(boxRect, roiRect)) {
            ++edgeDropped;

            continue;
        }
        candidates.push_back(
            FaceAnchor{box.x + box.width / 2.0, box.y + box.height / 2.0, static_cast<double>(box.width)});
    }
    const bool wide = face_lock::searchingWide(locked->second.state);
    const FaceLockDecision verdict = face_lock::decide(locked->second.state, candidates);
    SS_DIAG(FaceLock, "%s reason='%s' wide=%d candidates=%zu edge-dropped=%zu anchor=%.1f,%.1f",
            verdict.adopt ? "adopt" : "hold", verdict.reason.c_str(), wide ? 1 : 0, candidates.size(), edgeDropped,
            locked->second.state.lastAnchor.centerX, locked->second.state.lastAnchor.centerY);
    m_faceLockHunting = verdict.hunting;
    if (!verdict.hunting) {
        locked->second.anchorVerifiedAt = glfwGetTime();
    }
    if (face_lock::givenUp(locked->second.state)) {
        SS_DIAG(FaceLock, "gave up - removing region");
        removeLostFaceLock(m_faceLockProbe.forWindowIdentity);

        return;
    }
    if (verdict.adopt) {
        applyFaceLockRegion(locked->second.state);
    }
}

// The face could not be found for several seconds: the region dissolves
// instead of sitting somewhere wrong, and the user re-picks when ready.
void App::removeLostFaceLock(uint64_t identity)
{
    m_faceLocks.erase(identity);
    m_faceLockHunting = false;
    m_regionContentChangedAt = -1.0;
    m_attach.remove(identity);
    if (m_activeWindowIdentity == identity) {
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
    }
    m_attachDetachNotice = "face lost - region removed";
    m_attachNoticeUntil = glfwGetTime() + 5.0;
    setRegion(m_globalRegion);
    syncRegionBorder();
    m_lastActivity = glfwGetTime();
}

// The adopted anchor's region, applied through the same path as a border
// edit: the controller re-derives the stored screen-glued rectangle and the
// analysis region follows.
void App::applyFaceLockRegion(const FaceLockState& lock)
{
    if (!m_frameSize || m_frameSize->width <= 0 || m_frameSize->height <= 0) {
        return;
    }
    const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay());
    const auto windowGeom = windowGeometry(m_activeWindowIdentity);
    if (!geometry || !windowGeom || windowGeom->minimized) {
        return;
    }
    const LockRect target = face_lock::mapRegion(lock, lock.lastAnchor);
    const RegionOfInterest edited = percentFromLockRect(target, m_frameSize->width, m_frameSize->height);
    m_analysis.region = m_attach.editRegion(
        edited, AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height},
        AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints});
    m_analysisDirty = true;
}

// The idle tick, in slices, while windows are attached: a programmatic window
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
        const auto focusedNow = focusedAttachedWindow(foregroundApplicationPid(), m_attach.attachedIdentities());
        const uint64_t focusedAttachedIdentity = focusedNow && m_attach.isAttached(*focusedNow) ? *focusedNow : 0;
        if (focusedAttachedIdentity != m_activeWindowIdentity) {
            return;
        }
        if (m_activeWindowIdentity == 0 || !m_attachLastSeenRect) {
            continue;
        }
        const auto rect = windowGeometry(m_activeWindowIdentity);
        if (!rect) {
            return;  // closed: the frame body prunes it
        }
        if (rect->x != m_attachLastSeenRect->x || rect->y != m_attachLastSeenRect->y ||
            rect->width != m_attachLastSeenRect->width || rect->height != m_attachLastSeenRect->height) {
            onWindowMotion(WindowMotionSignal::Moved);

            return;
        }
        // A face-locked region's content churn - a pan under an idle loop -
        // takes the border down within a slice, not a whole tick.
        if (m_faceLocks.contains(m_activeWindowIdentity)) {
            probeRegionContentChange();
            if (regionContentUnsettled()) {
                hideRegionBorder();
            }
        }
    }
}

// A window rectangle as its display's percentages - the shape the
// window-candidate list and the edit-time veil speak.
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

// Sheds only the front attached window; the last one's detach is the full
// reset back to the screen.
void App::detachActiveWindow()
{
    if (m_attach.attachedCount() > 1 && m_attach.activeIdentity() != 0) {
        m_attach.remove(m_attach.activeIdentity());
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
    } else {
        resetToFullScreen();
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
    preferences.layoutOrientation = orientationToInt(m_view.orientation());
    preferences.layoutWeights = m_view.weightsSnapshot();
    preferences.layoutPresets = m_layoutPresets;
    preferences.layoutActiveSlot = m_activePresetSlot;
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
    m_captureController->service(glfwGetTime());
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
    if (m_captureController->permissionGranted() && !m_captureController->dead() && !m_regionPicking &&
        isFullScreen() && !m_attach.attached()) {
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
    // Cursor color, smoothed per scope with its own rhythm. On the captured
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
    const bool onCapturedDisplay = displayAtPoint(*cursor).value_or(0) == m_captureController->capturedDisplay();
    if (onCapturedDisplay && !m_captureController->dead() && m_frameSize) {
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
    drawScopePanes();
    drawStatusBar();
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

float App::statusBarHeight() const
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

        return;
    }
    if (m_captureController->dead()) {
        const std::string status = m_captureController->status();
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
    menuAction(menu, "Pin Colors...", MenuPinColor, false, shortcutLabel(m_shortcuts.pinColor));
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

void App::appendLayoutSubmenu(std::vector<NativeMenuItem>& menu)
{
    const LayoutOrientation current = m_view.orientation();
    menuSubmenu(menu, "Layout");
    menuAction(menu, "Automatic", MenuLayoutAuto, current == LayoutOrientation::Automatic);
    menuAction(menu, "Vertical (stacked)", MenuLayoutVertical, current == LayoutOrientation::Vertical);
    menuAction(menu, "Horizontal (side by side)", MenuLayoutHorizontal, current == LayoutOrientation::Horizontal);
    menuEndSubmenu(menu);
}

void App::appendPresetsSubmenu(std::vector<NativeMenuItem>& menu)
{
    // Each slot lists its saved summary or "empty"; the digit hint teaches the
    // load shortcut. Saving rides a nested submenu with the Shift+digit hint.
    menuSubmenu(menu, "Presets");
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        const LayoutPreset& preset = m_layoutPresets[static_cast<std::size_t>(slot - 1)];
        menuAction(menu, presetLabel(slot, preset).c_str(), MenuLoadPresetBase + slot, slot == m_activePresetSlot,
                   std::to_string(slot));
    }
    menuSeparator(menu);
    menuSubmenu(menu, "Save Current To");
    for (int slot = 1; slot <= LayoutPresetSlots; ++slot) {
        menuAction(menu, std::to_string(slot).c_str(), MenuSavePresetBase + slot, false,
                   "Shift+" + std::to_string(slot));
    }
    menuEndSubmenu(menu);
    menuEndSubmenu(menu);
}

void App::appendRegionAndAppSection(std::vector<NativeMenuItem>& menu)
{
    menuSeparator(menu);
    menuAction(menu, "Attach to Window...", MenuAttachWindow, false, shortcutLabel(m_shortcuts.attachWindow));
    menuAction(menu, "Draw Region...", MenuDrawRegion, false, shortcutLabel(m_shortcuts.drawRegion));
    if (supportsFaceDetection()) {
        menuAction(menu, "Attach to Face...", MenuAttachFace, false, shortcutLabel(m_shortcuts.attachFace));
    }
    menuAction(menu, "Watch Full Screen", MenuFullScreen, isFullScreen(), shortcutLabel(m_shortcuts.fullScreen));
    if (m_attach.attachedCount() > 1) {
        if (m_attach.activeIdentity() != 0) {
            menuAction(menu, "Detach Front Window", MenuDetachWindow, false);
        }
        menuAction(menu, "Detach All Windows", MenuDetachAll, false);
    } else if (m_attach.attached()) {
        menuAction(menu, "Detach from Window", MenuDetachWindow, false);
    }

    menuSeparator(menu);
    menuAction(menu, "Graticule", MenuToggleGraticule, m_view.graticule());

    menuSeparator(menu);
    // Support tooling in one clearly named place; every checkbox reads
    // the live truth, so a session started by the environment shows as
    // switched on and can be switched off here. Reset restores the
    // standard state however recording or visibility were enabled.
    menuSubmenu(menu, "Diagnostics");
    if (captureVisibilityToggleSupported()) {
        menuAction(menu, "Show in Screen Captures", MenuToggleCaptureVisibility, captureVisible());
    }
    menuAction(menu, "Record Diagnostic Log", MenuToggleDiagRecording, diagRecording());
    menuAction(menu, "Show Diagnostic Log", MenuShowDiagLog, false);
    menuSeparator(menu);
    menuAction(menu, "Reset to Defaults", MenuResetDiagnostics, false);
    menuEndSubmenu(menu);
    menuAction(menu, "Settings", MenuOpenSettings, false);
    menuAction(menu, "About SideScopes", MenuAbout, false);
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
    appendLayoutSubmenu(menu);
    appendPresetsSubmenu(menu);
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
    dispatchLayoutMenu(chosen);
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
    if (!isFullScreen()) {
        waitForBorderFreeFrame();
    }
    const std::vector<PickerDisplay> pickerDisplays = buildPickerDisplays();
    logPickerSuggestions(pickerDisplays);
    if (beginRegionPick(pickerDisplays, *m_wantRegionPick)) {
        m_regionPicking = true;
        m_regionPickIsPin = *m_wantRegionPick == RegionPickerMode::PinColor;
        // The streamed display's faces opened with the picker; scan the rest
        // in the background and deliver them as they land.
        launchDisplayFaceScans(pickerDisplays);
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
    // The suggestions, per display: the visible application windows, frontmost
    // first, plus, behind their own key, the faces the platform detector
    // finds. The streamed display's faces come from its live frame right
    // now; the other displays are scanned in the background (see
    // launchDisplayFaceScans) and their suggestions arrive later.
    const uint32_t streamed = m_captureController->capturedDisplay();
    std::vector<SuggestedRegion> faceSuggestions;
    m_faceCandidates.clear();
    if (supportsFaceDetection()) {
        (void)m_worker.withLatestFrame([&](const FrameView& view) {
            const auto geometry = geometryOfDisplay(streamed);
            const float pixelsPerPoint = geometry ? static_cast<float>(view.width / geometry->widthPoints) : 1.0f;
            const std::vector<IntRect> faces = detectFaces(view, pixelsPerPoint);
            faceSuggestions = buildFaceSuggestions(faces, view.width, view.height);
            // The raw boxes are remembered too, one candidate each: a confirmed
            // face pick anchors its lock on the detector's box, not the inset.
            const std::vector<FaceCandidate> candidates = buildFaceCandidates(faces, streamed, view.width, view.height);
            m_faceCandidates.insert(m_faceCandidates.end(), candidates.begin(), candidates.end());
            SS_DIAG(Suggestions, "display %u faces=%zu (streamed)", streamed, faceSuggestions.size());
        });
    }

    std::vector<PickerDisplay> pickerDisplays;
    // Remember every window and its identity so a confirmed window pick can
    // be turned into an attachment.
    m_windowCandidates.clear();
    for (const CaptureTarget& target : m_capture->listTargets()) {
        PickerDisplay entry;
        entry.displayId = target.displayId;
        entry.windows = windowSuggestionsFor(target.displayId);
        if (const auto geometry = geometryOfDisplay(target.displayId)) {
            for (const DesktopWindow& onScreen : onScreenWindows(target.displayId)) {
                const WindowGeometry rect{onScreen.x, onScreen.y, onScreen.width, onScreen.height, false, {}};
                m_windowCandidates.push_back({onScreen.windowIdentity, onScreen.ownerPid, onScreen.application,
                                              AttachWindowRect{onScreen.x, onScreen.y, onScreen.width, onScreen.height},
                                              displayPercentRect(rect, *geometry), target.displayId});
            }
        }
        if (target.displayId == streamed) {
            // The streamed display's faces are known now, from the live frame:
            // the picker opens with them and, empty or not, its scan is done.
            entry.faces = faceSuggestions;
            entry.facesScanned = true;
        }
        pickerDisplays.push_back(std::move(entry));
    }

    return pickerDisplays;
}

// One detached scan per non-streamed display, following the face-lock probe's
// discipline: a per-scan record the thread fills under a mutex, a ready flag,
// a wake, and a running flag the shutdown drain waits on. Opening the picker
// never blocks on these - only the streamed display's instant scan gates the
// open.
void App::launchDisplayFaceScans(const std::vector<PickerDisplay>& pickerDisplays)
{
    if (!supportsFaceDetection()) {
        return;
    }
    ++m_facePickGeneration;
    // Clear out the records of scans that have finished and been consumed; a
    // still-running one stays put - its detached thread holds a pointer in.
    std::erase_if(m_displayFaceScans, [](const std::unique_ptr<DisplayFaceScan>& scan) {
        return !scan->running.load() && !scan->ready.load();
    });
    const uint32_t streamed = m_captureController->capturedDisplay();
    for (const PickerDisplay& entry : pickerDisplays) {
        if (entry.displayId == streamed) {
            continue;  // scanned already, from the live frame
        }
        const auto geometry = geometryOfDisplay(entry.displayId);
        const double widthPoints = geometry ? geometry->widthPoints : 0.0;
        m_displayFaceScans.push_back(std::make_unique<DisplayFaceScan>());
        DisplayFaceScan* scan = m_displayFaceScans.back().get();
        scan->displayId = entry.displayId;
        scan->generation = m_facePickGeneration;
        scan->running.store(true);
        const uint32_t displayId = entry.displayId;
        std::thread([scan, displayId, widthPoints] {
            ScanResult scanned = scanDisplayForFaces(displayId, widthPoints);
            {
                std::lock_guard lock(scan->mutex);
                scan->faces = std::move(scanned.faces);
                scan->frameWidth = scanned.width;
                scan->frameHeight = scanned.height;
                scan->elapsedMs = scanned.elapsedMs;
            }
            scan->ready.store(true);
            // The wake goes out before the running flag clears: the shutdown
            // drain waits on running, and GLFW must still be alive to hear it.
            glfwPostEmptyEvent();
            scan->running.store(false);
        }).detach();
    }
}

// Drains every finished display scan into the open picker. Runs each frame,
// so a scan that lands after its picker closed is still consumed (and
// dropped) and its record retired.
void App::drainDisplayFaceScans()
{
    for (const std::unique_ptr<DisplayFaceScan>& scan : m_displayFaceScans) {
        if (scan->ready.load()) {
            consumeDisplayFaceScan(*scan);
            scan->ready.store(false);
        }
    }
    // Only a fully finished scan is removed: a running one's detached thread
    // still holds a pointer into it.
    std::erase_if(m_displayFaceScans, [](const std::unique_ptr<DisplayFaceScan>& scan) {
        return !scan->running.load() && !scan->ready.load();
    });
}

// Turns one landed scan into the display's face suggestions: the overlay
// boxes and the face candidates for a confirmed pick, both keyed to this
// display's own frame dimensions. A scan whose picker has closed or that a
// newer opening superseded is logged and dropped.
void App::consumeDisplayFaceScan(DisplayFaceScan& scan)
{
    std::vector<IntRect> boxes;
    int frameWidth = 0;
    int frameHeight = 0;
    double elapsedMs = 0.0;
    {
        std::lock_guard lock(scan.mutex);
        boxes = std::move(scan.faces);
        scan.faces.clear();
        frameWidth = scan.frameWidth;
        frameHeight = scan.frameHeight;
        elapsedMs = scan.elapsedMs;
    }
    SS_DIAG(Suggestions, "display %u scan faces=%zu elapsed=%.1fms", scan.displayId, boxes.size(), elapsedMs);
    if (!m_regionPicking || scan.generation != m_facePickGeneration) {
        return;
    }
    const std::vector<SuggestedRegion> suggestions = buildFaceSuggestions(boxes, frameWidth, frameHeight);
    const std::vector<FaceCandidate> candidates = buildFaceCandidates(boxes, scan.displayId, frameWidth, frameHeight);
    m_faceCandidates.insert(m_faceCandidates.end(), candidates.begin(), candidates.end());
    // Delivering the suggestions marks this display scanned: an empty list now
    // reads as "none found", and a failed grab (empty too) shows no boxes.
    updatePickerFaces(scan.displayId, suggestions);
}

// Recovers which window candidate a confirmed region names. The picker passes
// a window's exact rectangle through unchanged, so the match is a near-exact
// rectangle comparison; a freehand draw matches nothing.
const App::WindowCandidate* App::matchWindowCandidate(uint32_t displayId, const RegionOfInterest& region) const
{
    const WindowCandidate* best = nullptr;
    double bestDelta = 0.0;
    for (const WindowCandidate& candidate : m_windowCandidates) {
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

void App::logPickerSuggestions(const std::vector<PickerDisplay>& pickerDisplays)
{
    // Field diagnosis: exactly what the pipeline suggested, one line per
    // suggested window.
    if (!diagEnabled(DiagChannel::Suggestions)) {
        return;
    }
    for (const auto& entry : pickerDisplays) {
        for (const auto& suggestion : entry.windows) {
            SS_DIAG(Suggestions, "display %u suggestion '%s' %.1f,%.1f..%.1f,%.1f%%", entry.displayId,
                    suggestion.label.c_str(), suggestion.region.leftPercent, suggestion.region.topPercent,
                    suggestion.region.rightPercent, suggestion.region.bottomPercent);
        }
    }
}

void App::handleRegionBorderEdit()
{
    // The region border is live: dragging its edges, corners, or move tab
    // adjusts the region it currently outlines - the attached region of the
    // focused attached window, or the global one - with the scopes following.
    if (m_regionPicking) {
        m_attachBorderEditing = false;

        return;
    }
    const RegionBorderEdit edit = pollRegionBorderEdit();
    if (edit.editing && !m_attachBorderEditing) {
        // Latch what the border showed when the drag began: no focus race
        // can reroute the edit to the other region kind.
        m_attachBorderEditIdentity = m_activeWindowIdentity;
    }
    // While an attached border is dragged, a click-through veil dims
    // everything outside its window - the resize limit made visible.
    if (edit.editing && m_attachBorderEditIdentity != 0 && m_attachBorderEditIdentity == m_activeWindowIdentity) {
        const auto windowGeom = windowGeometry(m_activeWindowIdentity);
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
    } else if (edit.attachToggled) {
        toggleRegionAttach();
    } else if (edit.region) {
        applyBorderEdit(*edit.region);
    }
}

// The border's attach toggle. An attached region lets go of its window and
// becomes the global region in place; a global one attaches to the
// frontmost window under it. Explicit conversions only - the structural
// no-conversion rule is about drags and focus races, never this button.
void App::toggleRegionAttach()
{
    if (regionKind() == RegionKind::Attached) {
        const RegionOfInterest region = m_analysis.region;
        m_attach.detachAll();
        m_faceLocks.clear();
        m_faceLockHunting = false;
        m_regionContentChangedAt = -1.0;
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
        m_globalRegion = region;
        setRegion(region);
    } else {
        attachGlobalRegionToWindow();
    }
    m_lastActivity = glfwGetTime();
    syncRegionBorder();
}

// Attaching a global region: the frontmost on-screen window under the
// region's centre becomes its window, the rectangle staying exactly where
// it is. Over no window at all this is a no-op - predictable beats
// guessing a target.
void App::attachGlobalRegionToWindow()
{
    const uint32_t displayId = m_captureController->capturedDisplay();
    const auto geometry = geometryOfDisplay(displayId);
    if (!geometry || isFullScreen()) {
        return;
    }
    const RegionOfInterest region = m_globalRegion;
    const double centerX = (region.leftPercent + region.rightPercent) / 2.0;
    const double centerY = (region.topPercent + region.bottomPercent) / 2.0;
    for (const DesktopWindow& window : attachCandidateWindows(displayId)) {
        const WindowGeometry rect{window.x, window.y, window.width, window.height, false, {}};
        const RegionOfInterest windowRegion = displayPercentRect(rect, *geometry);
        if (centerX < windowRegion.leftPercent || centerX > windowRegion.rightPercent ||
            centerY < windowRegion.topPercent || centerY > windowRegion.bottomPercent) {
            continue;
        }
        adoptAttachedPick(window.windowIdentity, window.ownerPid,
                          m_attach.attach(window.windowIdentity, window.ownerPid, window.application,
                                          AttachWindowRect{window.x, window.y, window.width, window.height},
                                          AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints,
                                                            geometry->heightPoints},
                                          region));

        return;
    }
}

// The border's close affordances dismiss the region it outlines: the
// attached one detaches from its window only, the global one resets to
// full screen; the other attached windows keep their regions either way.
void App::dismissEditedBorder()
{
    if (regionKind() == RegionKind::Attached) {
        m_attach.remove(m_activeWindowIdentity);
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
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
    if (m_attachBorderEditIdentity != 0) {
        if (m_attachBorderEditIdentity != m_activeWindowIdentity) {
            return;
        }
        const auto geometry = geometryOfDisplay(m_captureController->capturedDisplay());
        const auto windowGeom = windowGeometry(m_activeWindowIdentity);
        if (!geometry || !windowGeom) {
            return;
        }
        // Attached: re-derive the window-relative fraction so the region
        // keeps following its window.
        applied = m_attach.editRegion(
            edited, AttachWindowRect{windowGeom->x, windowGeom->y, windowGeom->width, windowGeom->height},
            AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints});
        // A face-locked window's edit re-teaches the lock: the new rectangle
        // becomes the crop the face carries from here on.
        const auto lock = m_faceLocks.find(m_attachBorderEditIdentity);
        if (lock != m_faceLocks.end() && m_frameSize) {
            face_lock::rebindCrop(lock->second.state,
                                  lockRectFromPercent(applied, m_frameSize->width, m_frameSize->height));
        }
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
    // Color pinning never touches the region: clicks and drags deliver samples
    // to average, and a finish just puts things back.
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

    if (poll.pinnedPoint || poll.pinnedSample) {
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
            // dragged rectangle averages, the explicit way to ask for a swatch.
            const int pixelX = static_cast<int>(poll.pinnedPoint->xPercent / 100.0 * m_frameSize->width);
            const int pixelY = static_cast<int>(poll.pinnedPoint->yPercent / 100.0 * m_frameSize->height);
            pinned = m_worker.sampleFrameColor(pixelX, pixelY);
        } else if (poll.pinnedSample) {
            pinned = averageFrameColor(*poll.pinnedSample);
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
    // Live preview only for the captured display: previewing a suggestion on
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
            // Cancelled with Esc: detach every attached window and reset all
            // drawing to full screen. A cancel ordered by a tool switch is
            // not the user's Esc and resets nothing.
            resetToFullScreen();
        }
        m_regionPickSwallowCancel = false;
        syncRegionBorder();
        m_lastActivity = glfwGetTime();
    }
}

// The shared tail of both attached creations: the global region retires
// (one region kind at a time), the motion state starts fresh, the watch
// rebinds on the next follow step, and the picked window comes up so the
// border never wraps someone else's pixels.
void App::adoptAttachedPick(uint64_t identity, int64_t ownerPid, const RegionOfInterest& region)
{
    m_globalRegion = RegionOfInterest{};
    // A manual pick or draw replaces whatever face lock the window wore.
    m_faceLocks.erase(identity);
    m_attachedWindowMoving = false;
    m_attachGripActive = false;
    m_attachRegionMovedAt = -1.0;
    unwatchWindowMotion();
    m_activeWindowIdentity = 0;
    raiseWindow(identity, ownerPid);
    setRegion(region);
}

/// The quick start a window click hands back: the window rectangle pulled
/// in by a fixed margin - generous enough that the border chrome and label
/// strip clear the title bar and its buttons. Toolbars are the user's
/// resize job, never a guess.
constexpr double AttachQuickStartInsetPoints = 48.0;

RegionOfInterest quickStartRegion(const RegionOfInterest& window, const DisplayGeometry& display)
{
    const double widthPoints = (window.rightPercent - window.leftPercent) / 100.0 * display.widthPoints;
    const double heightPoints = (window.bottomPercent - window.topPercent) / 100.0 * display.heightPoints;
    const double inset = std::min({AttachQuickStartInsetPoints, widthPoints / 6.0, heightPoints / 6.0});
    RegionOfInterest region = window;
    region.leftPercent += inset / display.widthPoints * 100.0;
    region.rightPercent -= inset / display.widthPoints * 100.0;
    region.topPercent += inset / display.heightPoints * 100.0;
    region.bottomPercent -= inset / display.heightPoints * 100.0;

    return region;
}

// Field diagnosis for the window-pick mapping: every rectangle in the
// chain, on the suggestions channel.
void App::logAttachMapping(const WindowCandidate& picked, const RegionOfInterest& start) const
{
    SS_DIAG(Suggestions, "pick window=%llu list-rect=%.1f,%.1f %.1fx%.1f suggested=%.2f,%.2f..%.2f,%.2f%%",
            static_cast<unsigned long long>(picked.identity), picked.windowRect.x, picked.windowRect.y,
            picked.windowRect.width, picked.windowRect.height, picked.region.leftPercent, picked.region.topPercent,
            picked.region.rightPercent, picked.region.bottomPercent);
    if (const auto live = windowGeometry(picked.identity)) {
        SS_DIAG(Suggestions, "pick live-rect=%.1f,%.1f %.1fx%.1f minimized=%d", live->x, live->y, live->width,
                live->height, live->minimized ? 1 : 0);
    }
    SS_DIAG(Suggestions, "pick quick-start=%.2f,%.2f..%.2f,%.2f%%", start.leftPercent, start.topPercent,
            start.rightPercent, start.bottomPercent);
}

// A confirmed region that names a window attaches to it (or re-picks an
// attached one); a rectangle drawn in attach mode binds to the frontmost
// window under it; a freehand draw sets the global region.
void App::confirmPickedRegion(const RegionPickPoll& poll)
{
    const RegionOfInterest confirmed = *poll.confirmed;
    const WindowCandidate* picked = matchWindowCandidate(poll.displayId, confirmed);
    const auto geometry = geometryOfDisplay(poll.displayId);
    const auto display = geometry
                             ? std::optional<AttachDisplayRect>(AttachDisplayRect{
                                   geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints})
                             : std::nullopt;
    if (picked != nullptr && display && geometry) {
        // A window click quick-starts inset from the window's edges.
        const RegionOfInterest start = quickStartRegion(picked->region, *geometry);
        logAttachMapping(*picked, start);
        adoptAttachedPick(picked->identity, picked->ownerPid,
                          m_attach.attach(picked->identity, picked->ownerPid, picked->application, picked->windowRect,
                                          *display, start));

        return;
    }
    // A confirmed face suggestion attaches to the window under it.
    if (adoptFacePick(poll.displayId, confirmed)) {
        return;
    }
    // A rectangle drawn in attach mode binds to the frontmost window under
    // it; over no window at all it falls through to the global region.
    if (poll.attachesToWindow && display) {
        const WindowCandidate* host = windowContaining(poll.displayId, confirmed);
        if (host != nullptr) {
            adoptAttachedPick(host->identity, host->ownerPid,
                              m_attach.attach(host->identity, host->ownerPid, host->application, host->windowRect,
                                              *display, confirmed));

            return;
        }
    }
    // One region kind at a time: a global draw retires every attached
    // region.
    if (m_attach.attached()) {
        m_attach.detachAll();
        unwatchWindowMotion();
        m_activeWindowIdentity = 0;
        m_attachedWindowMoving = false;
        m_attachGripActive = false;
    }
    m_globalRegion = confirmed;
    setRegion(m_globalRegion);
}

// Recovers which face candidate a confirmed region names, by the same
// near-exact comparison the window match uses.
const FaceCandidate* App::matchFaceCandidate(uint32_t displayId, const RegionOfInterest& region) const
{
    constexpr double MatchTolerance = 1.0;

    for (const FaceCandidate& candidate : m_faceCandidates) {
        if (candidate.displayId != displayId) {
            continue;
        }
        const double delta = std::abs(candidate.region.leftPercent - region.leftPercent) +
                             std::abs(candidate.region.topPercent - region.topPercent) +
                             std::abs(candidate.region.rightPercent - region.rightPercent) +
                             std::abs(candidate.region.bottomPercent - region.bottomPercent);
        if (delta <= MatchTolerance) {
            return &candidate;
        }
    }

    return nullptr;
}

// The frontmost suggested window under a region's centre: the window a face
// pick attaches to.
const App::WindowCandidate* App::windowContaining(uint32_t displayId, const RegionOfInterest& region) const
{
    const double centerX = (region.leftPercent + region.rightPercent) / 2.0;
    const double centerY = (region.topPercent + region.bottomPercent) / 2.0;
    for (const WindowCandidate& candidate : m_windowCandidates) {
        if (candidate.displayId != displayId) {
            continue;
        }
        if (centerX >= candidate.region.leftPercent && centerX <= candidate.region.rightPercent &&
            centerY >= candidate.region.topPercent && centerY <= candidate.region.bottomPercent) {
            return &candidate;
        }
    }

    return nullptr;
}

// A confirmed face suggestion becomes an attachment on the window under it:
// the window's attachment carries the region between focus changes, and the
// lock follows the face within it. A face over no suggested window falls
// through to the plain global path.
bool App::adoptFacePick(uint32_t displayId, const RegionOfInterest& confirmed)
{
    const FaceCandidate* face = matchFaceCandidate(displayId, confirmed);
    if (face == nullptr) {
        return false;
    }
    const WindowCandidate* host = windowContaining(displayId, confirmed);
    const auto geometry = geometryOfDisplay(displayId);
    if (host == nullptr || !geometry) {
        return false;
    }
    const RegionOfInterest mapped = m_attach.attach(
        host->identity, host->ownerPid, host->application, host->windowRect,
        AttachDisplayRect{geometry->originX, geometry->originY, geometry->widthPoints, geometry->heightPoints},
        confirmed);
    adoptAttachedPick(host->identity, host->ownerPid, mapped);
    const FaceAnchor anchor{face->box.x + face->box.width / 2.0, face->box.y + face->box.height / 2.0,
                            static_cast<double>(face->box.width)};
    m_faceLocks[host->identity] =
        AppFaceLock{face_lock::makeLock(anchor, lockRectFromPercent(confirmed, face->frameWidth, face->frameHeight)),
                    host->windowRect, glfwGetTime()};
    SS_DIAG(FaceLock, "locked to '%s' anchor=%.1f,%.1f width=%.1f", host->application.c_str(), anchor.centerX,
            anchor.centerY, anchor.width);

    return true;
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
