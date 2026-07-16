// The SideScopes application shell, shared by every platform: a compact,
// always-on-top window stacking the enabled scopes. All analysis lives in
// the core library on its own thread; this file owns the interaction
// model (gestures, native menu, region selection) and preferences, while
// rendering and window chrome live behind the graphics seam.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "app/capture_controller.h"
#include "app/pin_board.h"
#include "app/scope_view.h"
#include "app/version.h"
#include "core/analysis_worker.h"
#include "core/color_lab.h"
#include "core/frame_mailbox.h"
#include "core/marker_smoother.h"
#include "core/preferences.h"
#include "core/region_suggestions.h"
#include "core/scopes/graticule.h"
#include "core/trace_intensity.h"
#include "imgui.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"
#include "platform/graphics.h"
#include "platform/native_menu.h"
#include "platform/region_selection.h"
#include "platform/screen_capture.h"
#include "sidescopes_version.h"

namespace {

using namespace sidescopes;

enum MenuAction
{
    MenuShowVectorscope = 1,
    MenuShowWaveform,
    MenuShowWaveformParade,
    MenuShowHistogram,
    MenuShowColorPicker,
    MenuWaveformStyleRgb = 10,
    MenuWaveformStyleLuma,
    MenuHistogramCombined,
    MenuHistogramPerChannel,
    MenuMatrixBt601 = 20,
    MenuMatrixBt709,
    MenuTraceBoosted,
    MenuTraceLinear,
    MenuWaveformStyleColoredLuma,
    MenuDrawRegion,
    MenuPickFaces,
    MenuZoom1,
    MenuZoom2,
    MenuZoom4,
    MenuSelectRegion = 30,
    MenuFullScreenRegion,
    MenuToggleGraticule = 40,
    MenuClearPinnedMarkers,
    MenuPickPinColor,
    MenuOpenSettings = 50,
    MenuQuit,
};

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

struct DrawnScope
{
    ImVec2 origin;
    ImVec2 size;
    float zoom = 1.0f;
};

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

ImVec2 at(const DrawnScope& scope, const NormalizedPoint& point)
{
    const float x = (point.x - 0.5f) * scope.zoom + 0.5f;
    const float y = (point.y - 0.5f) * scope.zoom + 0.5f;
    return ImVec2(scope.origin.x + x * scope.size.x, scope.origin.y + y * scope.size.y);
}

// Every scope speaks with one graticule voice: the same champagne gold
// at a handful of strengths. The scales used to be neutral gray while
// the vectorscope wore gold, which read as two different instruments.
constexpr ImU32 GraticuleMinor = IM_COL32(205, 172, 110, 70);
constexpr ImU32 GraticuleMajor = IM_COL32(205, 172, 110, 130);
constexpr ImU32 GraticuleAccent = IM_COL32(218, 175, 95, 180);
constexpr ImU32 GraticuleLabel = IM_COL32(226, 198, 145, 200);
constexpr ImU32 GraticuleSkinTone = IM_COL32(230, 170, 140, 160);

void drawVectorscopeOverlay(const DrawnScope& scope, const VectorscopeGraticule& graticule)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto strokeColor = [](GraticuleStroke stroke) -> ImU32 {
        switch (stroke) {
        case GraticuleStroke::GridMajor:
            return GraticuleMajor;
        case GraticuleStroke::Accent:
            return GraticuleAccent;
        case GraticuleStroke::SkinTone:
            return GraticuleSkinTone;
        case GraticuleStroke::Grid:
            break;
        }
        return GraticuleMinor;
    };

    for (const GraticuleLine& line : graticule.lines) {
        draw->AddLine(at(scope, line.from), at(scope, line.to), strokeColor(line.stroke),
                      line.stroke == GraticuleStroke::GridMajor ? 1.5f : 1.0f);
    }
    for (const GraticuleCircle& circle : graticule.circles) {
        draw->AddCircle(at(scope, circle.center), circle.radius * scope.size.x * scope.zoom, strokeColor(circle.stroke),
                        64);
    }
    for (const GraticuleTarget& target : graticule.targets) {
        const ImVec2 center = at(scope, target.center);
        const float box = target.primary ? 5.0f : 3.0f;
        draw->AddRect(ImVec2(center.x - box, center.y - box), ImVec2(center.x + box, center.y + box), GraticuleAccent);
        if (!target.label.empty()) {
            draw->AddText(ImVec2(center.x + 7, center.y - 7), GraticuleLabel, target.label.c_str());
        }
    }
}

void drawWaveformOverlay(const DrawnScope& scope)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool roomy = scope.size.y >= 140.0f;
    for (const WaveformScaleLine& line : buildWaveformScale()) {
        const float y = scope.origin.y + line.y * scope.size.y;
        const ImU32 color = line.major ? GraticuleMajor : GraticuleMinor;
        draw->AddLine(ImVec2(scope.origin.x, y), ImVec2(scope.origin.x + scope.size.x, y), color);
        if (line.major || roomy) {
            draw->AddText(ImVec2(scope.origin.x + 4, y + 1), GraticuleLabel, line.label.c_str());
        }
    }
}

void drawPointMarker(const DrawnScope& scope, const NormalizedPoint& point, ImU32 color)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = at(scope, point);
    draw->AddCircle(center, 5.0f, color, 0, 2.0f);
    draw->AddCircle(center, 6.5f, IM_COL32(0, 0, 0, 200), 0, 1.0f);
}

void drawLevelMarker(const DrawnScope& scope, float normalizedY, ImU32 color, float fromX = 0.0f, float toX = 1.0f)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float y = scope.origin.y + normalizedY * scope.size.y;
    draw->AddLine(ImVec2(scope.origin.x + fromX * scope.size.x, y), ImVec2(scope.origin.x + toX * scope.size.x, y),
                  color, 1.5f);
}

void drawValueMarker(const DrawnScope& scope, float normalizedX, ImU32 color, float fromY = 0.0f, float toY = 1.0f)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x = scope.origin.x + normalizedX * scope.size.x;
    draw->AddLine(ImVec2(x, scope.origin.y + fromY * scope.size.y), ImVec2(x, scope.origin.y + toY * scope.size.y),
                  color, 1.5f);
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
std::optional<TraceAdjustment> traceIntensityGesture(const DrawnScope& scope, TraceControl control, float intensity,
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

// Cursor markers for the three channels, merged where the values
// coincide: lines folding into one must read as the mix - gray for all
// three, yellow, magenta, or cyan for a pair - not as whichever channel
// happened to draw last.
struct ChannelMarker
{
    float value = 0.0f;  // 0..255
    ImU32 color = 0;
};

ImU32 channelMaskColor(int mask)
{
    switch (mask) {
    case 0b001:
        return IM_COL32(255, 90, 90, 230);  // red
    case 0b010:
        return IM_COL32(90, 255, 90, 230);  // green
    case 0b100:
        return IM_COL32(110, 110, 255, 230);  // blue
    case 0b011:
        return IM_COL32(255, 235, 90, 230);  // red+green: yellow
    case 0b101:
        return IM_COL32(255, 90, 255, 230);  // red+blue: magenta
    case 0b110:
        return IM_COL32(90, 235, 255, 230);  // green+blue: cyan
    default:
        return IM_COL32(235, 235, 235, 230);  // all three: gray
    }
}

int groupChannelMarkers(const FloatColor& color, ChannelMarker out[3])
{
    const float channels[3] = {color.r, color.g, color.b};
    constexpr float MergeEpsilon = 2.0f;
    bool grouped[3] = {false, false, false};
    int count = 0;
    for (int channel = 0; channel < 3; ++channel) {
        if (grouped[channel]) {
            continue;
        }
        int mask = 1 << channel;
        float sum = channels[channel];
        int members = 1;
        for (int other = channel + 1; other < 3; ++other) {
            if (grouped[other]) {
                continue;
            }
            if (std::abs(channels[other] - channels[channel]) <= MergeEpsilon) {
                grouped[other] = true;
                mask |= 1 << other;
                sum += channels[other];
                ++members;
            }
        }
        out[count++] = ChannelMarker{sum / static_cast<float>(members), channelMaskColor(mask)};
    }
    return count;
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
        // A simplified pointing hand: index finger up, palm with knuckle
        // notches, the shape of the pick-mode cursor.
        (void)a;
        // Traced from the classic cursor-hand outline: tall index left of
        // center, three knuckle stubs descending to the right, the thumb
        // web sweeping diagonally down-left, and a flat cuff. Outlined,
        // like the rest of the icon row.
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
    } else if (icon == RegionIcon::Crosshair) {
        // The draw-mode crosshair: long thin beams, small center gap.
        const auto beam = [&](float dx, float dy) {
            draw->AddLine(ImVec2(center.x + dx * 1.25f, center.y + dy * 1.25f),
                          ImVec2(center.x + dx * 7.5f, center.y + dy * 7.5f), color, 1.4f);
        };
        beam(0.0f, -1.0f);
        beam(0.0f, 1.0f);
        beam(-1.0f, 0.0f);
        beam(1.0f, 0.0f);
    } else if (icon == RegionIcon::Face) {
        // A face: head outline, two eyes, a smile arc.
        draw->AddCircle(center, 7.5f, color, 0, 1.4f);
        draw->AddCircleFilled(ImVec2(center.x - 2.8f, center.y - 2.0f), 1.1f, color);
        draw->AddCircleFilled(ImVec2(center.x + 2.8f, center.y - 2.0f), 1.1f, color);
        ImVec2 smile[5];
        for (int i = 0; i < 5; ++i) {
            const float angle = (0.30f + 0.10f * i) * 3.14159265f;
            smile[i] = ImVec2(center.x + 3.3f * std::cos(angle), center.y + 3.3f * std::sin(angle));
        }
        draw->AddPolyline(smile, 5, color, ImDrawFlags_None, 1.4f);
    } else if (icon == RegionIcon::Dropper) {
        // The classic pipette silhouette, filled so it reads at chip
        // size: round bulb, a separate wider collar band, a long
        // tapering tip, and a drop fallen just past it.
        draw->AddCircleFilled(ImVec2(center.x + 4.4f, center.y - 4.4f), 2.4f, color);
        draw->AddLine(ImVec2(center.x + 1.7f, center.y - 1.7f), ImVec2(center.x + 3.0f, center.y - 3.0f), color, 4.2f);
        draw->AddTriangleFilled(ImVec2(center.x - 5.3f, center.y + 5.3f), ImVec2(center.x + 1.3f, center.y + 0.7f),
                                ImVec2(center.x - 0.7f, center.y - 1.3f), color);
        draw->AddCircleFilled(ImVec2(center.x - 6.6f, center.y + 6.6f), 1.0f, color);
    } else {
        // Two arrows expanding to opposite corners, the fullscreen idiom.
        const auto arrow = [&](ImVec2 from, ImVec2 to, float headX, float headY) {
            draw->AddLine(from, to, color, stroke);
            draw->AddLine(to, ImVec2(to.x + headX * 3.5f, to.y), color, stroke);
            draw->AddLine(to, ImVec2(to.x, to.y + headY * 3.5f), color, stroke);
        };
        arrow(ImVec2(center.x - 1.5f, center.y + 1.5f), ImVec2(a.x + 0.5f, b.y - 0.5f), 1, -1);
        arrow(ImVec2(center.x + 1.5f, center.y - 1.5f), ImVec2(b.x - 0.5f, a.y + 0.5f), -1, 1);
    }
    ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

// How many faces the platform detector saw on the captured screen at the
// last check: -1 before any check. Refreshed in the background when the
// application gains focus, so the face button can present itself honestly
// - dimmed when there is currently nothing to pick. The state can go
// stale while the user works elsewhere, so the button only dims, never
// disables: pressing F always detects freshly.
std::atomic<int> g_facesOnScreen{-1};
std::atomic<bool> g_faceCheckRequested{false};
std::atomic<bool> g_faceCheckRunning{false};

// Minimizing is "get out of my way": the region border follows the
// window down and returns on restore. The flag wakes the frame loop's
// border sync when the iconified state flips either way.
std::atomic<bool> g_iconifyChanged{false};

void refreshFacePresence(AnalysisWorker& worker, uint32_t displayId)
{
    if (!supportsFaceDetection()) {
        return;
    }
    if (g_faceCheckRunning.exchange(true)) {
        return;
    }
    // Detection takes long enough to hitch a frame, so it runs on a copy
    // of the latest frame in a background thread.
    auto pixels = std::make_shared<std::vector<uint8_t>>();
    int width = 0;
    int height = 0;
    worker.withLatestFrame([&](const FrameView& view) {
        width = view.width;
        height = view.height;
        pixels->resize(static_cast<std::size_t>(view.height) * view.strideBytes);
        std::memcpy(pixels->data(), view.bgra, pixels->size());
    });
    if (width == 0 || height == 0) {
        g_faceCheckRunning.store(false);
        return;
    }
    float pixelsPerPoint = 1.0f;
    if (const auto geometry = geometryOfDisplay(displayId)) {
        pixelsPerPoint = static_cast<float>(width / geometry->widthPoints);
    }
    std::thread([pixels, width, height, pixelsPerPoint] {
        const FrameView view{pixels->data(), width * 4, width, height, ColorSpaceHint::Srgb, 0};
        g_facesOnScreen.store(static_cast<int>(detectFaces(view, pixelsPerPoint).size()));
        g_faceCheckRunning.store(false);
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

// The fixed-width companion for values whose glyphs must align - hex
// codes most of all; null when no system monospace font was found, and
// the interface font stands in.
ImFont* g_monospaceFont = nullptr;

void loadInterfaceFont(GLFWwindow* window)
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
    for (const std::string& path : monospaceFontFiles()) {
        if ((g_monospaceFont = io.Fonts->AddFontFromFileTTF(path.c_str(), 13.0f, &config))) {
            break;
        }
    }
    (void)loaded;
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

    // The monitor carrying most of the window; the primary when none is.
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

    const int availableWidth = std::max(1, workWidth - frameLeft - frameRight);
    const int availableHeight = std::max(1, workHeight - frameTop - frameBottom);
    const int clampedWidth = std::min(width, availableWidth);
    const int clampedHeight = std::min(height, availableHeight);
    const int minX = workX + frameLeft;
    const int maxX = workX + workWidth - frameRight - clampedWidth;
    const int minY = workY + frameTop;
    const int maxY = workY + workHeight - frameBottom - clampedHeight;
    const int clampedX = std::max(minX, std::min(x, maxX));
    const int clampedY = std::max(minY, std::min(y, maxY));
    if (clampedWidth != width || clampedHeight != height) {
        glfwSetWindowSize(window, clampedWidth, clampedHeight);
    }
    if (clampedX != x || clampedY != y) {
        glfwSetWindowPos(window, clampedX, clampedY);
    }
}

// Frontmost, non-auxiliary windows on a display become region suggestions.
// A window mostly inside a larger one of the same app is auxiliary chrome
// (Lightroom panels and overlays); one mostly hidden behind another is
// skipped.
std::vector<SuggestedRegion> windowSuggestionsFor(uint32_t displayId)
{
    std::vector<WindowRegion> windowRegions;
    const auto geometry = geometryOfDisplay(displayId);
    if (geometry) {
        // Frontmost windows only, skipping ones mostly hidden behind
        // fronter windows: the user is not scoping what they cannot
        // see, and invisible system windows have no business here.
        const std::vector<DesktopWindow> onScreen = onScreenWindows(displayId);
        const auto containedFraction = [](const DesktopWindow& inner, const DesktopWindow& outer) {
            const double left = std::max(inner.x, outer.x);
            const double top = std::max(inner.y, outer.y);
            const double right = std::min(inner.x + inner.width, outer.x + outer.width);
            const double bottom = std::min(inner.y + inner.height, outer.y + outer.height);
            if (right <= left || bottom <= top) {
                return 0.0;
            }
            return (right - left) * (bottom - top) / (inner.width * inner.height);
        };
        std::vector<DesktopWindow> visibleWindows;
        std::vector<DesktopWindow> auxiliaryWindows;
        for (const DesktopWindow& candidate : onScreen) {
            constexpr int MaxWindowSuggestions = 5;
            if (static_cast<int>(visibleWindows.size()) >= MaxWindowSuggestions) {
                break;
            }
            // A window living mostly inside a bigger window of the
            // same application is an auxiliary surface - Lightroom
            // draws its panels and its loupe info overlay as
            // borderless windows over the main one - and picking it
            // is never meant. It is remembered as chrome: the
            // detector masks it out, so panels and overlays neither
            // spawn candidates nor interrupt the photograph's
            // borders.
            bool auxiliary = false;
            for (const DesktopWindow& other : onScreen) {
                if (&other == &candidate || other.application != candidate.application) {
                    continue;
                }
                if (other.width * other.height <= candidate.width * candidate.height) {
                    continue;
                }
                if (containedFraction(candidate, other) > 0.9) {
                    auxiliary = true;
                    break;
                }
            }
            if (auxiliary) {
                auxiliaryWindows.push_back(candidate);
                continue;
            }
            bool mostlyCovered = false;
            for (const DesktopWindow& front : visibleWindows) {
                if (containedFraction(candidate, front) > 0.8) {
                    mostlyCovered = true;
                    break;
                }
            }
            if (!mostlyCovered) {
                visibleWindows.push_back(candidate);
            }
        }

        for (const DesktopWindow& visible : visibleWindows) {
            WindowRegion region;
            region.region.leftPercent =
                std::clamp((visible.x - geometry->originX) / geometry->widthPoints * 100.0, 0.0, 100.0);
            region.region.topPercent =
                std::clamp((visible.y - geometry->originY) / geometry->heightPoints * 100.0, 0.0, 100.0);
            region.region.rightPercent =
                std::clamp((visible.x + visible.width - geometry->originX) / geometry->widthPoints * 100.0, 0.0, 100.0);
            region.region.bottomPercent = std::clamp(
                (visible.y + visible.height - geometry->originY) / geometry->heightPoints * 100.0, 0.0, 100.0);
            region.application = visible.application;
            windowRegions.push_back(std::move(region));
        }
    }
    return buildRegionSuggestions(windowRegions);
}

// The histogram pane draws the filled texture, strokes each channel's curve
// over it at display resolution, then adds the graticule and cursor-value
// markers.
void drawHistogram(const ScopeTexture& texture, const AnalysisWorker::Output& output, HistogramStyle style,
                   bool showGraticule, const std::optional<FloatColor>& markerColor)
{
    // No intensity gesture here: the histogram's scale adjusts
    // itself, the way every editor draws it.
    const DrawnScope scope = drawScopeImage(texture, false);
    // The curve outline strokes at display resolution over the
    // filled texture: baked into the texture it would stretch
    // anisotropically with the pane - thick on flats, thin on
    // slopes. Sampled through the same spline the fill uses, so
    // line and fill edge agree.
    if (output.histogramOutline.size() == static_cast<std::size_t>(3) * Histogram::Bins) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(scope.origin, ImVec2(scope.origin.x + scope.size.x, scope.origin.y + scope.size.y), true);
        const bool bands = style == HistogramStyle::PerChannel;
        const int samples = std::clamp(static_cast<int>(scope.size.x), 128, 2 * Histogram::Bins);
        static std::vector<ImVec2> points;
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
                    0.5f * t *
                        (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
                if (p1 <= 0.0f && p2 <= 0.0f) {
                    height = 0.0f;
                }
                height = std::clamp(height, 0.0f, 1.0f);
                // Empty stretches ride the baseline: the outline
                // stays one continuous reading of the channel,
                // rather than blinking away wherever the plot
                // touches zero. Kept just inside the band so the
                // stroke survives the clip.
                const float y = std::min(bandTop + (1.0f - height) * bandHeight, bandTop + bandHeight - 1.0f);
                points.push_back(ImVec2(scope.origin.x + (sample + 0.5f) * scope.size.x / samples, y));
            }
            if (points.size() >= 2) {
                draw->AddPolyline(points.data(), static_cast<int>(points.size()), channelMaskColor(1 << channel),
                                  ImDrawFlags_None, 1.6f);
            }
        }
        draw->PopClipRect();
    }
    if (showGraticule) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int quarter = 0; quarter <= 4; ++quarter) {
            const float x = scope.origin.x + scope.size.x * quarter / 4.0f;
            draw->AddLine(ImVec2(x, scope.origin.y), ImVec2(x, scope.origin.y + scope.size.y),
                          quarter % 2 == 0 ? GraticuleMajor : GraticuleMinor);
        }
    }
    if (markerColor) {
        if (style == HistogramStyle::PerChannel) {
            // Each channel's marker stays a single color inside
            // its own band.
            const float channels[3] = {markerColor->r, markerColor->g, markerColor->b};
            for (int channel = 0; channel < 3; ++channel) {
                drawValueMarker(scope, channels[channel] / 255.0f, channelMaskColor(1 << channel), channel / 3.0f,
                                (channel + 1) / 3.0f);
            }
        } else {
            ChannelMarker markers[3];
            const int count = groupChannelMarkers(*markerColor, markers);
            for (int i = 0; i < count; ++i) {
                drawValueMarker(scope, markers[i].value / 255.0f, markers[i].color);
            }
        }
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
void pushHexFont()
{
    if (g_monospaceFont) {
        ImGui::PushFont(g_monospaceFont);
    }
}

void popHexFont()
{
    if (g_monospaceFont) {
        ImGui::PopFont();
    }
}

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

void drawColorPicker(const std::optional<FloatColor>& liveColor, PinBoard& pins)
{
    // Three size tiers, few and spaced so resizing feels like
    // deliberate steps: a strip, a compact comparator, the full
    // reference deck. Order never changes - comparator, values,
    // pins - and only the comparator absorbs extra height; when
    // space runs out, pin detail goes first and the live readout
    // is the last thing standing.
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    if (pins.comparator() >= static_cast<int>(pins.size())) {
        pins.selectComparator(-1);
    }
    if (!liveColor) {
        ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (area.y - lineHeight) / 2.0f)));
        const char* hint = "no color under the cursor yet";
        const float width = ImGui::CalcTextSize(hint).x;
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
        ImGui::TextDisabled("%s", hint);
        return;
    }
    const FloatColor& color = *liveColor;
    const int red = static_cast<int>(std::lround(std::clamp(color.r, 0.0f, 255.0f)));
    const int green = static_cast<int>(std::lround(std::clamp(color.g, 0.0f, 255.0f)));
    const int blue = static_cast<int>(std::lround(std::clamp(color.b, 0.0f, 255.0f)));
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", red, green, blue);
    // Every value owns a column sized for its widest form and
    // right-aligns inside it - the toolbar's cure for layouts
    // that twitch as digits come and go.
    const float labelColumn = ImGui::CalcTextSize("R").x;
    const float percentColumn = ImGui::CalcTextSize("100%").x;
    const float columnGap = ImGui::CalcTextSize(" ").x;
    // One channel column plus its trailing gaps, repeated three times across a
    // row of values.
    const float channelStride = labelColumn + columnGap + percentColumn + 2 * columnGap;
    // Hex measures in the fixed-width font, where every seven-glyph code is the
    // same width, so this one figure serves both hero halves and the fallback.
    pushHexFont();
    const float hexWidth = ImGui::CalcTextSize(hex).x;
    popHexFont();
    const auto swatchColor = [](const FloatColor& source) {
        return ImVec4(source.r / 255.0f, source.g / 255.0f, source.b / 255.0f, 1.0f);
    };
    const auto pinHexOf = [&](std::size_t index, char* buffer) {
        std::snprintf(buffer, 8, "#%02X%02X%02X", static_cast<int>(std::lround(pins.color(index).r)),
                      static_cast<int>(std::lround(pins.color(index).g)),
                      static_cast<int>(std::lround(pins.color(index).b)));
    };
    // Signed values read cleanly in the fixed-width font, where the plus sign
    // sits level with the digits and the line does not jiggle as numbers change.
    // The interface font's plus rides low against the digits; the mono font
    // aligns them.
    const auto monoWidth = [](const char* text) {
        pushHexFont();
        const float measured = ImGui::CalcTextSize(text).x;
        popHexFont();

        return measured;
    };
    const auto monoTextDisabled = [](const char* text) {
        pushHexFont();
        ImGui::TextDisabled("%s", text);
        popHexFont();
    };
    const auto monoText = [](const char* text) {
        pushHexFont();
        ImGui::TextUnformatted(text);
        popHexFont();
    };

    // The comparator: the live color, split against the selected
    // pin when one is loaded. Touching halves make small casts
    // visible where separated swatches hide them. The split
    // never depends on the tier - only its height does.
    const bool tiny = area.y < 120.0f;
    const bool full = area.y >= 240.0f;
    // With nothing pinned there is no deck to reserve for, and an
    // uncapped comparator absorbs the pane instead of leaving a
    // dead black field beneath the values.
    const float deckReserve = pins.empty() ? 0.0f : (full ? 4.0f * lineHeight : lineHeight);
    const float reserved = 2.0f * lineHeight + deckReserve + style.ItemSpacing.y;
    const float heroCap = pins.empty() ? area.y : (full ? 220.0f : 64.0f);
    const float heroHeight = tiny ? lineHeight * 1.5f : std::clamp(area.y - reserved, 48.0f, heroCap);
    const bool split = pins.hasComparator();
    const float heroWidth = area.x;
    const ImVec2 heroOrigin = ImGui::GetCursorScreenPos();
    const float valuesStart = ImGui::GetCursorPosX();
    // The on-swatch readout is a bottom-anchored block of four rows - R, G and B
    // percentages, then hex - laid out in fixed columns and mirrored about the
    // seam so the two halves compare row by row.
    const float pad = 8.0f;
    const float rowHeight = lineHeight;
    const float seamX = heroOrigin.x + heroWidth / 2.0f;
    const float blockWidth = labelColumn + columnGap + percentColumn;
    const float blockBottom = heroOrigin.y + heroHeight - pad;
    const float blockTop = blockBottom - 4.0f * rowHeight;
    // A split half carries the readout only when it is tall enough for four rows
    // and wide enough to hold the block clear of its corner label; hex can be
    // the widest row, so it drives the width test. Otherwise the values fall to
    // the rows below the hero.
    const float blockExtent = std::max(blockWidth, hexWidth);
    const bool onSwatch = split && heroHeight >= 6.0f * lineHeight &&
                          heroWidth / 2.0f >= blockExtent + 2.0f * pad + ImGui::CalcTextSize("LIVE").x;
    // Solo, one line along the swatch foot carries the same reading when the
    // hero is tall enough and the whole line fits across it.
    const bool soloOnSwatch =
        !split && heroHeight >= 3.5f * lineHeight && 3.0f * channelStride + hexWidth + 2.0f * pad <= heroWidth;

    // Ink and its legibility shadow both follow the color beneath: dark ink over
    // a light shadow on light colors, light ink over a dark shadow on dark ones.
    const auto labelInk = [](const FloatColor& under) {
        const float luma = (54.0f * under.r + 183.0f * under.g + 19.0f * under.b) / 256.0f;
        return luma > 140.0f ? IM_COL32(0, 0, 0, 170) : IM_COL32(255, 255, 255, 180);
    };
    const auto labelShadow = [](const FloatColor& under) {
        const float luma = (54.0f * under.r + 183.0f * under.g + 19.0f * under.b) / 256.0f;
        return luma > 140.0f ? IM_COL32(255, 255, 255, 64) : IM_COL32(0, 0, 0, 115);
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // One string over its shadow, in an optional font: the hex rows pass the
    // fixed-width font, everything else draws in the interface font. The font is
    // pushed rather than sized by hand so global UI scale applies once.
    const auto swatchText = [&](const ImVec2& pos, ImU32 ink, ImU32 shadow, const char* text, ImFont* font = nullptr) {
        if (font) {
            ImGui::PushFont(font, font->LegacySize);
        }
        draw->AddText(ImVec2(pos.x, pos.y + 1.0f), shadow, text);
        draw->AddText(pos, ink, text);
        if (font) {
            ImGui::PopFont();
        }
    };
    // One hero half's readout: labels in a fixed column, percentages
    // right-aligned to the seam, hex on the fourth row. The left half mirrors
    // the right about the seam so equal values sit at equal heights.
    const auto drawSwatchBlock = [&](const FloatColor& swatch, bool leftHalf) {
        const ImU32 ink = labelInk(swatch);
        const ImU32 shadow = labelShadow(swatch);
        const float channels[3] = {swatch.r, swatch.g, swatch.b};
        const char* labels[3] = {"R", "G", "B"};
        for (int channel = 0; channel < 3; ++channel) {
            const float y = blockTop + static_cast<float>(channel) * rowHeight;
            char value[8];
            std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55f);
            const float valueWidth = ImGui::CalcTextSize(value).x;
            const float labelX = leftHalf ? seamX - pad - blockWidth : seamX + pad;
            const float valueX = leftHalf ? seamX - pad - valueWidth : seamX + pad + blockWidth - valueWidth;
            swatchText(ImVec2(labelX, y), ink, shadow, labels[channel]);
            swatchText(ImVec2(valueX, y), ink, shadow, value);
        }
        char blockHex[8];
        std::snprintf(blockHex, sizeof(blockHex), "#%02X%02X%02X",
                      static_cast<int>(std::lround(std::clamp(swatch.r, 0.0f, 255.0f))),
                      static_cast<int>(std::lround(std::clamp(swatch.g, 0.0f, 255.0f))),
                      static_cast<int>(std::lround(std::clamp(swatch.b, 0.0f, 255.0f))));
        const float hexY = blockTop + 3.0f * rowHeight;
        const float hexX = leftHalf ? seamX - pad - hexWidth : seamX + pad;
        swatchText(ImVec2(hexX, hexY), ink, shadow, blockHex, g_monospaceFont);
    };

    // The live swatch renders but never copies: the sample follows the cursor,
    // so a click would destroy the very color it shows.
    ImGui::ColorButton("##picker-live", swatchColor(color),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(split ? heroWidth / 2.0f : heroWidth, heroHeight));
    if (split) {
        char pinHex[8];
        pinHexOf(static_cast<std::size_t>(pins.comparator()), pinHex);
        ImGui::SameLine(0.0f, 0.0f);
        if (ImGui::ColorButton("##picker-reference", swatchColor(pins.comparatorColor()),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(heroWidth / 2.0f, heroHeight))) {
            ImGui::SetClipboardText(pinHex);
        }
        ImGui::SetItemTooltip("pinned  %s - click to copy", pinHex);
        if (!tiny) {
            draw->AddText(ImVec2(heroOrigin.x + 5, heroOrigin.y + 3), labelInk(color), "LIVE");
            const float pinLabel = ImGui::CalcTextSize("PIN").x;
            draw->AddText(ImVec2(heroOrigin.x + heroWidth - pinLabel - 5, heroOrigin.y + 3),
                          labelInk(pins.comparatorColor()), "PIN");
            if (onSwatch) {
                drawSwatchBlock(color, true);
                drawSwatchBlock(pins.comparatorColor(), false);
            }
        }
    }
    if (soloOnSwatch) {
        const ImU32 ink = labelInk(color);
        const ImU32 shadow = labelShadow(color);
        draw->AddText(ImVec2(heroOrigin.x + 5, heroOrigin.y + 3), ink, "LIVE");
        const float baselineY = heroOrigin.y + heroHeight - pad - rowHeight;
        const float startX = heroOrigin.x + pad;
        const float channels[3] = {color.r, color.g, color.b};
        const char* labels[3] = {"R", "G", "B"};
        for (int channel = 0; channel < 3; ++channel) {
            const float columnStart = startX + static_cast<float>(channel) * channelStride;
            char value[8];
            std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55f);
            swatchText(ImVec2(columnStart, baselineY), ink, shadow, labels[channel]);
            const float valueX = columnStart + labelColumn + columnGap + percentColumn - ImGui::CalcTextSize(value).x;
            swatchText(ImVec2(valueX, baselineY), ink, shadow, value);
        }
        swatchText(ImVec2(startX + 3.0f * channelStride, baselineY), ink, shadow, hex, g_monospaceFont);
    }

    // The same reading below the hero when the swatches are too small to carry
    // it: the percent a photographer reads at a glance, with hex alongside for
    // the exact code. Plain text now - the hero half already declines to copy,
    // and this echoes it. A row of decimal codes said what hex already says.
    if (!onSwatch && !soloOnSwatch) {
        const float liveChannels[3] = {color.r, color.g, color.b};
        const char* channelLabels[3] = {"R", "G", "B"};
        for (int channel = 0; channel < 3; ++channel) {
            const float columnStart = valuesStart + channel * channelStride;
            if (channel > 0) {
                ImGui::SameLine(columnStart);
            } else {
                ImGui::SetCursorPosX(columnStart);
            }
            ImGui::TextUnformatted(channelLabels[channel]);
            char text[8];
            std::snprintf(text, sizeof(text), "%.0f%%", liveChannels[channel] / 2.55f);
            ImGui::SameLine(columnStart + labelColumn + columnGap + percentColumn - ImGui::CalcTextSize(text).x);
            ImGui::TextUnformatted(text);
        }
        ImGui::SameLine(0.0f, 0.0f);
        pushHexFont();
        if (ImGui::GetContentRegionAvail().x >= hexWidth + 12.0f) {
            ImGui::SameLine(area.x - hexWidth);
            ImGui::TextUnformatted(hex);
        } else {
            ImGui::NewLine();
            ImGui::TextUnformatted(hex);
        }
        popHexFont();
    }

    // One quiet line below the hero: the colorist's difference on the left - how
    // much lighter, more saturated, further around the wheel - and a match
    // percentage on the right. The triplet drops out whole when the line is too
    // narrow to seat it clear of the match, which always stays.
    if (split && !tiny) {
        const LabColor liveLab = labFromSrgb(color);
        const LabColor pinLab = labFromSrgb(pins.comparatorColor());
        const ColorDifference difference = differenceFrom(pinLab, liveLab);
        // Match is 100 minus the CIEDE2000 distance, floored: 100% appears only
        // when the two colors are essentially identical, and 0% anchors at black
        // against white, the farthest two colors can be.
        const int matchPercent = static_cast<int>(std::clamp(100.0f - difference.deltaE, 0.0f, 100.0f));
        char matchValue[8];
        std::snprintf(matchValue, sizeof(matchValue), "%d%%", matchPercent);
        const float matchValueX = area.x - monoWidth(matchValue);
        const float matchLabelX = matchValueX - columnGap - ImGui::CalcTextSize("Match").x;
        char matchHelp[192];
        std::snprintf(matchHelp, sizeof(matchHelp),
                      "similarity to the live color: 100%% is identical, 0%% is as far apart as black and white "
                      "(CIEDE2000 difference %.1f) - sRGB assumed",
                      difference.deltaE);
        // Three groups - lightness, chroma, hue - each a label in the interface
        // font over a value in the fixed-width font. The value follows its
        // label directly, so a short value stays beside its own letter instead
        // of drifting toward the next one; the group starts keep fixed strides
        // sized for the widest value, so the letters never move as the numbers
        // change, and the fixed-width digits keep the signs level.
        const char* diffLabels[3] = {"L", "C", "H"};
        const float diffValues[3] = {difference.lightness, difference.chroma, difference.hue};
        const float diffValueColumn = monoWidth("+199");
        float tripletWidth = 0.0f;
        for (int component = 0; component < 3; ++component) {
            tripletWidth += ImGui::CalcTextSize(diffLabels[component]).x + columnGap + diffValueColumn;
            if (component < 2) {
                tripletWidth += 2.0f * columnGap;
            }
        }
        const char* diffHelp = "live minus pinned: lightness, chroma, and hue weighted by chroma - sRGB assumed";
        if (valuesStart + tripletWidth + columnGap <= matchLabelX) {
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
                ImGui::SameLine(groupX + labelWidth + columnGap);
                monoTextDisabled(value);
                ImGui::SetItemTooltip("%s", diffHelp);
                groupX += labelWidth + columnGap + diffValueColumn + 2.0f * columnGap;
            }
            ImGui::SameLine(matchLabelX);
        } else {
            ImGui::SetCursorPosX(matchLabelX);
        }
        ImGui::TextDisabled("Match");
        ImGui::SetItemTooltip("%s", matchHelp);
        ImGui::SameLine(matchValueX);
        monoText(matchValue);
        ImGui::SetItemTooltip("%s", matchHelp);
    }

    // The pins: a full reference deck when there is room, a chip
    // rail when there is not. Clicking loads the comparator;
    // hex copies; the right-click menu manages.
    if (pins.empty()) {
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, lineHeight * 0.35f));
    int removePin = -1;
    if (full) {
        ImGui::BeginChild("##pin-deck", ImVec2(0, 0));
        // Fixed left part: the remove cross at the very edge, a clear gap, the
        // swatch, then the hex in its own column. The numeric columns follow at
        // fixed strides. Width decisions use the child's own content region:
        // the pane width measured outside ignores this child's scrollbar, and
        // the difference clipped the last column under it.
        const float deckWidth = ImGui::GetContentRegionAvail().x;
        const float hexColumn = monoWidth("#DDDDDD");
        const float swatchX = lineHeight + 3.0f * columnGap;
        const float hexX = swatchX + lineHeight + columnGap;
        const float leftPartEnd = hexX + hexColumn;
        // Numeric columns, each sized for its widest value or its header,
        // whichever is wider. Match reads a percentage, L/C/H the CIELAB
        // difference, R/G/B the per-channel percentage.
        const float matchCol = std::max(monoWidth("100%"), ImGui::CalcTextSize("Match").x);
        const char* lchLabels[3] = {"L", "C", "H"};
        const char* rgbLabels[3] = {"R", "G", "B"};
        float lchCol[3];
        float rgbCol[3];
        for (int column = 0; column < 3; ++column) {
            lchCol[column] = std::max(monoWidth("+199"), ImGui::CalcTextSize(lchLabels[column]).x);
            rgbCol[column] = std::max(monoWidth("+100%"), ImGui::CalcTextSize(rgbLabels[column]).x);
        }
        const float lchGroupWidth = lchCol[0] + lchCol[1] + lchCol[2] + 2.0f * (2.0f * columnGap);
        const float rgbGroupWidth = rgbCol[0] + rgbCol[1] + rgbCol[2] + 2.0f * (2.0f * columnGap);
        // Progressive disclosure: Match first, then L/C/H, then R/G/B, each group
        // admitted only if the whole block still clears the hex column. A group
        // shows in every row or none, so the decision is made once here.
        const float blockGap = 3.0f * columnGap;
        float numericBlockWidth = 0.0f;
        const auto admitGroup = [&](float groupWidth) {
            const float tentative =
                numericBlockWidth + (numericBlockWidth > 0.0f ? 3.0f * columnGap : 0.0f) + groupWidth;
            if (leftPartEnd + blockGap + tentative <= deckWidth) {
                numericBlockWidth = tentative;

                return true;
            }

            return false;
        };
        const bool showMatch = admitGroup(matchCol);
        const bool showLch = showMatch && admitGroup(lchGroupWidth);
        const bool showRgb = showLch && admitGroup(rgbGroupWidth);
        // Right edges of every visible column, walked left to right from the
        // block's left edge; the header and each row share them. The block
        // anchors just past the hex column rather than against the pane's far
        // edge, so a wide pane keeps the table together instead of stretching
        // a gap through its middle.
        float matchRight = 0.0f;
        float lchRight[3] = {0.0f, 0.0f, 0.0f};
        float rgbRight[3] = {0.0f, 0.0f, 0.0f};
        float walk = leftPartEnd + blockGap;
        if (showMatch) {
            matchRight = walk + matchCol;
            walk = matchRight;
        }
        if (showLch) {
            walk += 3.0f * columnGap;
            for (int column = 0; column < 3; ++column) {
                if (column > 0) {
                    walk += 2.0f * columnGap;
                }
                lchRight[column] = walk + lchCol[column];
                walk = lchRight[column];
            }
        }
        if (showRgb) {
            walk += 3.0f * columnGap;
            for (int column = 0; column < 3; ++column) {
                if (column > 0) {
                    walk += 2.0f * columnGap;
                }
                rgbRight[column] = walk + rgbCol[column];
                walk = rgbRight[column];
            }
        }
        const char* matchTip =
            "similarity to the live color: 100% is identical, 0% is as far apart as black and "
            "white (CIEDE2000) - sRGB assumed";
        const char* lchTip =
            "lightness, chroma, and hue difference, live minus pinned - hue weighted by chroma, "
            "sRGB assumed";
        const char* rgbTip = "channel difference, live minus pinned";
        // Header row: each label centers over the ink of a typical value - a
        // sign and two digits, right-aligned - rather than over the column box,
        // whose left slack exists only for the widest values. Nothing sits
        // above the cross, swatch, or hex.
        const float lchTypical = monoWidth("+34");
        const float rgbTypical = monoWidth("+34%");
        const float matchTypical = monoWidth("77%");
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
        if (showMatch) {
            headerCell(matchRight, matchTypical, "Match", matchTip);
        }
        if (showLch) {
            headerCell(lchRight[0], lchTypical, "L", lchTip);
            headerCell(lchRight[1], lchTypical, "C", lchTip);
            headerCell(lchRight[2], lchTypical, "H", lchTip);
        }
        if (showRgb) {
            headerCell(rgbRight[0], rgbTypical, "R", rgbTip);
            headerCell(rgbRight[1], rgbTypical, "G", rgbTip);
            headerCell(rgbRight[2], rgbTypical, "B", rgbTip);
        }
        for (std::size_t index = 0; index < pins.size(); ++index) {
            char pinId[24];
            std::snprintf(pinId, sizeof(pinId), "##pin-%d", static_cast<int>(index));
            char pinHex[8];
            pinHexOf(index, pinHex);
            const bool selected = static_cast<int>(index) == pins.comparator();
            const float rowPosY = ImGui::GetCursorPosY();
            const float textDrop = (lineHeight - ImGui::GetFontSize()) / 2.0f;
            // The remove cross leads the row: a frameless glyph, quiet gray until
            // hovered, red at the moment of intent, then a clear gap before the
            // swatch so the two never trade misclicks.
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
            cross->AddLine(ImVec2(crossCenter.x - arm, crossCenter.y - arm),
                           ImVec2(crossCenter.x + arm, crossCenter.y + arm), crossInk, 1.4f);
            cross->AddLine(ImVec2(crossCenter.x - arm, crossCenter.y + arm),
                           ImVec2(crossCenter.x + arm, crossCenter.y - arm), crossInk, 1.4f);
            // The swatch: click loads it into the comparator, right-click manages.
            ImGui::SameLine(swatchX);
            if (ImGui::ColorButton(pinId, swatchColor(pins.color(index)),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                   ImVec2(lineHeight, lineHeight))) {
                pins.selectComparator(selected ? -1 : static_cast<int>(index));
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                pins.manage(static_cast<int>(index));
                ImGui::OpenPopup("##pinned-menu");
            }
            ImGui::SetItemTooltip(selected ? "click to unload from the comparator"
                                           : "click to compare against the live color");
            if (selected) {
                // A white ring inside a dark one reads on any pin
                // color; the gold rim vanished on skin tones.
                const ImVec2 lo = ImGui::GetItemRectMin();
                const ImVec2 hi = ImGui::GetItemRectMax();
                ImDrawList* ringDraw = ImGui::GetWindowDrawList();
                ringDraw->AddRect(ImVec2(lo.x - 1, lo.y - 1), ImVec2(hi.x + 1, hi.y + 1), IM_COL32(0, 0, 0, 220), 0.0f,
                                  0, 2.0f);
                ringDraw->AddRect(lo, hi, IM_COL32(235, 235, 235, 235), 0.0f, 0, 1.5f);
            }
            // Hex, centered against the taller swatch, click to copy.
            ImGui::SameLine(hexX);
            ImGui::SetCursorPosY(rowPosY + textDrop);
            pushHexFont();
            ImGui::TextUnformatted(pinHex);
            popHexFont();
            if (ImGui::IsItemClicked()) {
                ImGui::SetClipboardText(pinHex);
            }
            ImGui::SetItemTooltip("click to copy");
            // Numeric columns: each value right-aligned in its column and
            // vertically centered like the hex.
            const ColorDifference pinDiff = differenceFrom(labFromSrgb(pins.color(index)), labFromSrgb(color));
            const auto numericCell = [&](float colRight, const char* value, const char* tip) {
                ImGui::SameLine(colRight - monoWidth(value));
                ImGui::SetCursorPosY(rowPosY + textDrop);
                monoTextDisabled(value);
                ImGui::SetItemTooltip("%s", tip);
            };
            if (showMatch) {
                char match[8];
                std::snprintf(match, sizeof(match), "%d%%",
                              static_cast<int>(std::clamp(100.0f - pinDiff.deltaE, 0.0f, 100.0f)));
                char matchHelp[192];
                std::snprintf(matchHelp, sizeof(matchHelp),
                              "similarity to the live color: 100%% is identical, 0%% is as far apart as black and "
                              "white (CIEDE2000 difference %.1f) - sRGB assumed",
                              pinDiff.deltaE);
                numericCell(matchRight, match, matchHelp);
            }
            if (showLch) {
                const float lchValues[3] = {pinDiff.lightness, pinDiff.chroma, pinDiff.hue};
                for (int column = 0; column < 3; ++column) {
                    char value[8];
                    std::snprintf(value, sizeof(value), "%+d", static_cast<int>(std::lround(lchValues[column])));
                    numericCell(lchRight[column], value, lchTip);
                }
            }
            if (showRgb) {
                const float pinChannels[3] = {pins.color(index).r, pins.color(index).g, pins.color(index).b};
                const float liveChannels[3] = {color.r, color.g, color.b};
                for (int column = 0; column < 3; ++column) {
                    char value[8];
                    std::snprintf(value, sizeof(value), "%+.0f%%",
                                  (liveChannels[column] - pinChannels[column]) / 2.55f);
                    numericCell(rgbRight[column], value, rgbTip);
                }
            }
        }
        drawPinnedMenu(pins);
        ImGui::EndChild();
    } else {
        for (std::size_t index = 0; index < pins.size(); ++index) {
            char pinId[24];
            std::snprintf(pinId, sizeof(pinId), "##pin-%d", static_cast<int>(index));
            char pinHex[8];
            pinHexOf(index, pinHex);
            const bool selected = static_cast<int>(index) == pins.comparator();
            if (index > 0) {
                ImGui::SameLine(0.0f, 4.0f);
            }
            if (ImGui::ColorButton(pinId, swatchColor(pins.color(index)),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                   ImVec2(lineHeight, lineHeight))) {
                pins.selectComparator(selected ? -1 : static_cast<int>(index));
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                pins.manage(static_cast<int>(index));
                ImGui::OpenPopup("##pinned-menu");
            }
            ImGui::SetItemTooltip("%s - click to compare, right-click to manage", pinHex);
            if (selected) {
                const ImVec2 lo = ImGui::GetItemRectMin();
                const ImVec2 hi = ImGui::GetItemRectMax();
                ImDrawList* ringDraw = ImGui::GetWindowDrawList();
                ringDraw->AddRect(ImVec2(lo.x - 1, lo.y - 1), ImVec2(hi.x + 1, hi.y + 1), IM_COL32(0, 0, 0, 220), 0.0f,
                                  0, 2.0f);
                ringDraw->AddRect(lo, hi, IM_COL32(235, 235, 235, 235), 0.0f, 0, 1.5f);
            }
        }
        drawPinnedMenu(pins);
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

}  // namespace

int main()
{
    if (!glfwInit()) {
        return 1;
    }

    const Preferences startup = loadPreferences(preferencesFilePath());
    const VersionInfo versionInfo = describeVersion(SIDESCOPES_VERSION, SIDESCOPES_GIT_DESCRIBE);

    std::unique_ptr<GraphicsBackend> graphics = createGraphicsBackend();
    graphics->setWindowHints();
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    // On Windows the window follows its monitor's scale, so it keeps its
    // physical size when dragged between differently scaled monitors;
    // macOS ignores the hint (scaling lives in the framebuffer there).
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    // Hidden until the saved placement is applied: geometry settles
    // before the first paint, and no intermediate rectangle flashes.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(startup.windowWidth, startup.windowHeight, "SideScopes", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    restoreWindowPlacement(window, startup);
    glfwShowWindow(window);
    // A development build wears its version in the title bar; a release keeps
    // the plain name.
    if (versionInfo.development) {
        glfwSetWindowTitle(window, ("SideScopes " + versionInfo.display).c_str());
    }
    // Installed before the ImGui backend so it chains this callback
    // instead of being replaced by it.
    glfwSetWindowFocusCallback(window, [](GLFWwindow*, int focused) {
        if (focused) {
            g_faceCheckRequested.store(true);
        }
    });
    glfwSetWindowIconifyCallback(window, [](GLFWwindow*, int) { g_iconifyChanged.store(true); });
    // The close button and Cmd+Q quit; intercepting the close to hide
    // instead swallowed BOTH, because the quit request reaches the
    // application as a window close. Dismissal belongs to Cmd+W and
    // Cmd+H alone.

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is ours to persist
    ImGui::StyleColorsDark();
    applyTheme();
    loadInterfaceFont(window);
    float uiScale = computeUiScale(window);
    const auto applyUiScale = [&uiScale](float scale) {
        uiScale = scale;
        // Scaling an already scaled style would compound, so rebuild from
        // the base theme each time.
        applyTheme();
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleMain = scale;
    };
    if (uiScale != 1.0f) {
        applyUiScale(uiScale);
    }
    if (!graphics->init(window)) {
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // --- capture and analysis ---
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    auto capture = createScreenCaptureSource();

    CaptureController captureController(*capture, mailbox);
    // The display under this window's center: full-screen capture is a
    // promise about the screen the user can see the scopes on.
    const auto displayOfWindow = [&]() -> std::optional<uint32_t> {
        int windowX = 0;
        int windowY = 0;
        int windowWidth = 0;
        int windowHeight = 0;
        glfwGetWindowPos(window, &windowX, &windowY);
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        return displayAtPoint(DesktopPoint{windowX + windowWidth / 2.0, windowY + windowHeight / 2.0});
    };
    if (captureController.requestPermission()) {
        captureController.requestDisplay(displayOfWindow().value_or(0));
        captureController.start();
    }

    // --- state, seeded from preferences ---
    AnalysisSettings analysis;
    analysis.vectorscope.gain = startup.vectorscopeGain;
    analysis.vectorscope.samplingStride = startup.vectorscopeStride;
    analysis.vectorscope.matrix = startup.matrix;
    analysis.vectorscope.response = startup.traceResponse;
    analysis.waveform.gain = startup.waveformGain;
    analysis.waveform.samplingStride = startup.waveformStride;
    analysis.histogram.samplingStride = startup.histogramStride;
    analysis.waveform.mode = startup.waveformMode;
    analysis.histogram.style = startup.histogramPerChannel ? HistogramStyle::PerChannel : HistogramStyle::Combined;
    bool analysisDirty = true;

    ScopeView view;
    view.restoreStack(startup.scopeStack);
    view.setGraticule(startup.showGraticule);
    view.setZoom(startup.vectorscopeZoom);
    view.setIntensity(TraceControl::Vectorscope,
                      intensityFromTraceGain(analysis.vectorscope.gain, VectorscopeIntensityShift));
    view.setIntensity(TraceControl::Waveform, intensityFromTraceGain(analysis.waveform.gain));
    view.setSmoothing(TraceControl::Vectorscope, startup.vectorscopeSmoothingMs);
    view.setSmoothing(TraceControl::Waveform, startup.waveformSmoothingMs);
    TraceFlash flash;
    analysis.enabledScopes = view.enabledMask();
    // Shortcuts come from the preferences file: the key handler acts on
    // them and the context menu displays them, resolved once here.
    const ShortcutBindings shortcuts = startup.shortcuts;
    const auto keyFor = [](const std::string& name) -> ImGuiKey {
        if (name == "Escape") {
            return ImGuiKey_Escape;
        }
        if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'A'));
        }
        return ImGuiKey_None;
    };
    const auto shortcutLabel = [](const std::string& name) -> std::string { return name == "Escape" ? "Esc" : name; };
    bool showSettings = false;
    // Reference colors pinned on the vectorscope (session-scoped): pin a
    // corrected skin tone, then match the next photo against it. References
    // come from two places: what the cursor reads (P), and the region's
    // average (Shift+P) - select a face, pin its skin.
    PinBoard pins;

    // Live region picking: while the overlay is up, the frame loop polls it
    // and previews the indicated region on the scopes; cancelling resets
    // to the full screen.
    bool regionPicking = false;
    // Whether the active pick is the pin tool - the two families never
    // morph into each other - and whether its cancellation was ordered by
    // a tool switch, which must not reset the region the way a user's Esc
    // on a region pick deliberately does.
    bool regionPickIsPin = false;
    bool regionPickSwallowCancel = false;

    // Projection-only engine instances kept in sync with the analysis
    // settings; they never accumulate, they only place overlays and markers.
    Vectorscope projectionVectorscope;
    Waveform projectionWaveform;

    MarkerSmoother vectorscopeMarker;
    MarkerSmoother waveformMarker;

    // Scope textures are drawn the same frame their scope becomes
    // visible, possibly before anything was ever uploaded - and a fresh
    // GPU texture holds whatever memory the driver recycled into it, so
    // every texture starts blanked to the scopes' black.
    const auto createBlankTexture = [&](int width, int height) {
        auto texture = graphics->createScopeTexture(width, height);
        ScopeImage blank;
        blank.width = width;
        blank.height = height;
        blank.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
        for (std::size_t i = 3; i < blank.rgba.size(); i += 4) {
            blank.rgba[i] = 255;
        }
        texture->upload(blank);
        return texture;
    };
    std::unique_ptr<ScopeTexture> vectorscopeTexture = createBlankTexture(Vectorscope::Size, Vectorscope::Size);
    std::unique_ptr<ScopeTexture> waveformTexture = createBlankTexture(Waveform::Columns, Waveform::Levels);
    std::unique_ptr<ScopeTexture> waveformParadeTexture = createBlankTexture(Waveform::Columns, Waveform::Levels);
    std::unique_ptr<ScopeTexture> histogramTexture = createBlankTexture(Histogram::ImageWidth, Histogram::Height);
    // Adaptive scope detail: panes are measured at draw time (stacking
    // splits the window, so the pane is what matters, not the window),
    // and desired resolutions are debounced so a live resize does not
    // thrash engine reallocation. The upload path recreates a texture
    // whenever its image changes dimensions.
    ImVec2 panePoints[5] = {};
    int pendingColumns = 0;
    int pendingImageHeight = 0;
    int pendingVectorscope = 0;
    int pendingHistWidth = 0;
    int pendingHistHeight = 0;
    double detailPendingSince = 0.0;
    const auto uploadScope = [&](std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image) {
        if (image.width <= 0 || image.height <= 0) {
            return;
        }
        if (image.rgba.size() < static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
            return;
        }
        if (texture->width() != image.width || texture->height() != image.height) {
            texture = graphics->createScopeTexture(image.width, image.height);
        }
        texture->upload(image);
    };

    worker.start();
    warmFaceDetection();

    uint64_t outputVersion = 0;
    AnalysisWorker::Output output;
    double lastActivity = glfwGetTime();

    // Waking the display or unlocking the session can leave the stream a
    // zombie; the wake signal forces the controller to restart it.
    observeSystemWake([&captureController] { captureController.markStale(); });
    double nextPreferencesSave = -1.0;
    DesktopPoint lastCursor{-1.0, -1.0};

    // The freshest cross-display sample: the async sampler's callback may
    // land on any thread, and may still be in flight at shutdown, so the
    // state it writes is shared ownership.
    struct ScreenSample
    {
        std::mutex mutex;
        std::optional<FloatColor> color;
    };

    auto screenSample = std::make_shared<ScreenSample>();
    double nextScreenSample = 0.0;

    const auto isFullRegion = [&] {
        return analysis.region.leftPercent <= 0.0 && analysis.region.topPercent <= 0.0 &&
               analysis.region.rightPercent >= 100.0 && analysis.region.bottomPercent >= 100.0;
    };
    const auto syncRegionBorder = [&] {
        if (captureController.capturedDisplay() == 0) {
            return;
        }
        if (isFullRegion() || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            hideRegionBorder();
        } else {
            showRegionBorder(captureController.capturedDisplay(), analysis.region);
        }
    };
    // The patch a pin-mode click samples, in interface points; the pin
    // picker uses the same span, so the chip previews what a click pins.
    constexpr double PinSamplePoints = 14.0;
    // Averages a display-percent area of the latest frame: the pin
    // modes' sample. Photographs are textured, so pins come from areas,
    // never single pixels.
    const auto averageFrameArea = [&](const RegionOfInterest& area) -> std::optional<FloatColor> {
        std::optional<FloatColor> color;
        worker.withLatestFrame([&](const FrameView& view) {
            const int left = std::clamp(static_cast<int>(area.leftPercent / 100.0 * view.width), 0, view.width);
            const int right = std::clamp(static_cast<int>(area.rightPercent / 100.0 * view.width), 0, view.width);
            const int top = std::clamp(static_cast<int>(area.topPercent / 100.0 * view.height), 0, view.height);
            const int bottom = std::clamp(static_cast<int>(area.bottomPercent / 100.0 * view.height), 0, view.height);
            if (right <= left || bottom <= top) {
                return;
            }
            // A stride caps the work on huge drags; the average barely
            // moves past a hundred thousand samples.
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
        return color;
    };
    // Resets all selection: a pending pick and the drawn region alike.
    // The border sync rides the analysis-dirty path.
    const auto resetRegionToFull = [&] {
        cancelRegionPick();
        analysis.region = RegionOfInterest{};
        analysisDirty = true;
    };
    // Escape pressed while this application is active but focusless (a
    // border grab activates without focusing); consumed by the frame
    // loop like any other cross-thread signal.
    std::atomic<bool> orphanEscape{false};
    observeEscapeWithoutKeyWindow([&orphanEscape] { orphanEscape.store(true); });
    const auto persistPreferences = [&] {
        Preferences preferences;
        preferences.vectorscopeGain = analysis.vectorscope.gain;
        preferences.waveformGain = analysis.waveform.gain;
        preferences.vectorscopeStride = analysis.vectorscope.samplingStride;
        preferences.waveformStride = analysis.waveform.samplingStride;
        preferences.vectorscopeSmoothingMs = view.smoothing(TraceControl::Vectorscope);
        preferences.waveformSmoothingMs = view.smoothing(TraceControl::Waveform);
        preferences.matrix = analysis.vectorscope.matrix;
        preferences.traceResponse = analysis.vectorscope.response;
        preferences.histogramStride = analysis.histogram.samplingStride;
        preferences.waveformMode = analysis.waveform.mode;
        preferences.histogramPerChannel = analysis.histogram.style == HistogramStyle::PerChannel;
        preferences.scopeStack = view.stackLetters();
        preferences.showGraticule = view.graticule();
        preferences.vectorscopeZoom = view.zoom();
        preferences.shortcuts = shortcuts;
        glfwGetWindowPos(window, &preferences.windowX, &preferences.windowY);
        glfwGetWindowSize(window, &preferences.windowWidth, &preferences.windowHeight);
        savePreferences(preferences, preferencesFilePath());
    };
    syncRegionBorder();

    while (!glfwWindowShouldClose(window)) {
        // Idle: with no new output, no cursor motion, and no interaction,
        // wait for events at a slow tick instead of spinning at refresh.
        if (glfwGetTime() - lastActivity > 0.5) {
            glfwWaitEventsTimeout(0.1);
        } else {
            glfwPollEvents();
        }

        // Capture is a service that dies (lock screen, display sleep);
        // restarting it is our job.
        if (g_faceCheckRequested.exchange(false)) {
            refreshFacePresence(worker, captureController.capturedDisplay());
        }
        if (g_iconifyChanged.exchange(false)) {
            syncRegionBorder();
            lastActivity = glfwGetTime();
        }
        if (orphanEscape.exchange(false)) {
            resetRegionToFull();
            lastActivity = glfwGetTime();
        }

        captureController.service(glfwGetTime());

        // With no region drawn, capture follows the display this window
        // sits on: the fallback stays predictable - you always scope the
        // screen you can see the scopes on. A drawn region pins capture
        // to its own display regardless of where the window goes.
        if (captureController.permissionGranted() && !captureController.dead() && !regionPicking && isFullRegion()) {
            const auto homeDisplay = displayOfWindow();
            if (homeDisplay && *homeDisplay != captureController.capturedDisplay()) {
                captureController.requestDisplay(*homeDisplay);
                if (captureController.start()) {
                    lastActivity = glfwGetTime();
                }
            }
        }

        // The window may have moved to a monitor with a different scale.
        const float currentScale = computeUiScale(window);
        if (currentScale != uiScale) {
            applyUiScale(currentScale);
            lastActivity = glfwGetTime();
        }

        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth == 0 || framebufferHeight == 0) {
            continue;
        }
        if (!graphics->beginFrame(framebufferWidth, framebufferHeight)) {
            continue;
        }

        if (worker.fetchOutput(outputVersion, output)) {
            if (view.shows(ScopeGlyph::Vectorscope)) {
                uploadScope(vectorscopeTexture, output.vectorscopeImage);
            }
            if (view.shows(ScopeGlyph::Waveform)) {
                uploadScope(waveformTexture, output.waveformImage);
            }
            if (view.shows(ScopeGlyph::WaveformParade)) {
                uploadScope(waveformParadeTexture, output.waveformParadeImage);
            }
            if (view.shows(ScopeGlyph::Histogram)) {
                uploadScope(histogramTexture, output.histogramImage);
            }
            lastActivity = glfwGetTime();
        }

        // Publish our own window rectangle (frame pixels, generous chrome
        // margins) so analysis masks it out of change detection.
        const auto frameSize = worker.latestFrameSize();
        if (frameSize && captureController.capturedDisplay() != 0) {
            if (const auto geometry = geometryOfDisplay(captureController.capturedDisplay())) {
                int windowX = 0, windowY = 0, windowW = 0, windowH = 0;
                glfwGetWindowPos(window, &windowX, &windowY);
                glfwGetWindowSize(window, &windowW, &windowH);
                const double scaleX = frameSize->width / geometry->widthPoints;
                const double scaleY = frameSize->height / geometry->heightPoints;
                // The chrome margins are 100%-scale units like the rest of
                // the interface, so they grow with the monitor's scale.
                const IntRect selfWindow{static_cast<int>((windowX - geometry->originX - 8 * uiScale) * scaleX),
                                         static_cast<int>((windowY - geometry->originY - 42 * uiScale) * scaleY),
                                         static_cast<int>((windowW + 16 * uiScale) * scaleX),
                                         static_cast<int>((windowH + 58 * uiScale) * scaleY)};
                if (selfWindow.x != analysis.maskedWindow.x || selfWindow.y != analysis.maskedWindow.y ||
                    selfWindow.width != analysis.maskedWindow.width ||
                    selfWindow.height != analysis.maskedWindow.height) {
                    analysis.maskedWindow = selfWindow;
                    analysisDirty = true;
                }
            }
        }

        // Cursor color, smoothed per scope with its own rhythm. On the
        // tracked display it reads the capture stream's frame; on every
        // other display a throttled one-shot sample keeps the readout,
        // the markers, and the picker pane alive - even while capture
        // itself is paused.
        std::optional<FloatColor> vectorscopeColor;
        std::optional<FloatColor> waveformColor;
        if (captureController.capturedDisplay() != 0) {
            if (const auto cursor = globalCursorPosition()) {
                if (std::abs(cursor->x - lastCursor.x) + std::abs(cursor->y - lastCursor.y) > 0.5) {
                    lastCursor = *cursor;
                    lastActivity = glfwGetTime();
                }
                std::optional<FloatColor> sampled;
                const bool onTrackedDisplay =
                    displayAtPoint(*cursor).value_or(0) == captureController.capturedDisplay();
                if (onTrackedDisplay && !captureController.dead() && frameSize) {
                    if (const auto geometry = geometryOfDisplay(captureController.capturedDisplay())) {
                        const int pixelX = static_cast<int>((cursor->x - geometry->originX) * frameSize->width /
                                                            geometry->widthPoints);
                        const int pixelY = static_cast<int>((cursor->y - geometry->originY) * frameSize->height /
                                                            geometry->heightPoints);
                        sampled = worker.sampleFrameColor(pixelX, pixelY);
                    }
                } else {
                    if (glfwGetTime() > nextScreenSample) {
                        nextScreenSample = glfwGetTime() + 0.05;
                        sampleScreenColorAsync(*cursor, [screenSample](std::optional<FloatColor> color) {
                            if (!color) {
                                return;
                            }
                            std::lock_guard lock(screenSample->mutex);
                            screenSample->color = color;
                        });
                    }
                    std::lock_guard lock(screenSample->mutex);
                    sampled = screenSample->color;
                }
                if (sampled) {
                    vectorscopeMarker.setTimeConstant(view.smoothing(TraceControl::Vectorscope));
                    waveformMarker.setTimeConstant(view.smoothing(TraceControl::Waveform));
                    vectorscopeColor = vectorscopeMarker.update(*sampled, io.DeltaTime);
                    waveformColor = waveformMarker.update(*sampled, io.DeltaTime);
                }
            }
        }

        // --- adaptive scope detail ---
        // Resolution follows the pane a scope actually gets, and never
        // exceeds what the region can populate: more columns than the
        // region has pixels only spreads samples thin, and a finer
        // chroma grid than the sample count can fill reads as noise.
        {
            int windowW = 0;
            int windowH = 0;
            glfwGetWindowSize(window, &windowW, &windowH);
            const float density = windowW > 0 ? static_cast<float>(framebufferWidth) / windowW : 1.0f;
            const auto pane = [&](ScopeGlyph kind) {
                const ImVec2& points = panePoints[static_cast<int>(kind)];
                return ImVec2(points.x * density, points.y * density);
            };
            int regionWidth = 0;
            if (frameSize) {
                const IntRect regionPixels = analysis.region.toPixels(frameSize->width, frameSize->height);
                regionWidth = regionPixels.width;
            }

            int wantColumns = analysis.waveform.columns;
            int wantHeight = analysis.waveform.imageHeight;
            if (view.shows(ScopeGlyph::Waveform) || view.shows(ScopeGlyph::WaveformParade)) {
                const float wfWidth = std::max(pane(ScopeGlyph::Waveform).x, pane(ScopeGlyph::WaveformParade).x);
                const float wfHeight = std::max(pane(ScopeGlyph::Waveform).y, pane(ScopeGlyph::WaveformParade).y);
                wantColumns = wfWidth >= 1400.0f ? 2048 : wfWidth >= 500.0f ? 1024 : 512;
                if (regionWidth > 0) {
                    wantColumns = std::min(wantColumns, regionWidth >= 2048 ? 2048 : regionWidth >= 1024 ? 1024 : 512);
                }
                wantHeight = wfHeight >= 560.0f ? 512 : WaveformLevels;
            }
            int wantHistWidth = analysis.histogram.imageWidth;
            int wantHistHeight = analysis.histogram.imageHeight;
            if (view.shows(ScopeGlyph::Histogram)) {
                // Near one texture pixel per screen pixel keeps the
                // outline's width even on flats and steep slopes alike.
                const ImVec2 scopePane = pane(ScopeGlyph::Histogram);
                wantHistWidth = scopePane.x >= 1400.0f ? 2048 : scopePane.x >= 500.0f ? 1024 : 512;
                wantHistHeight = scopePane.y >= 560.0f ? 768 : 384;
            }
            int wantVectorscope = analysis.vectorscope.size;
            if (view.shows(ScopeGlyph::Vectorscope)) {
                // Purely a display resolution: accumulation stays on the
                // 256-code grid and a finer image is interpolated from
                // it, so a sparse region costs nothing extra.
                const ImVec2 scopePane = pane(ScopeGlyph::Vectorscope);
                const float extent = std::min(scopePane.x, scopePane.y);
                wantVectorscope = extent >= 480.0f ? 512 : 256;
            }

            const bool differs =
                wantColumns != analysis.waveform.columns || wantHeight != analysis.waveform.imageHeight ||
                wantVectorscope != analysis.vectorscope.size || wantHistWidth != analysis.histogram.imageWidth ||
                wantHistHeight != analysis.histogram.imageHeight;
            if (!differs) {
                pendingColumns = 0;
            } else if (pendingColumns != wantColumns || pendingImageHeight != wantHeight ||
                       pendingVectorscope != wantVectorscope || pendingHistWidth != wantHistWidth ||
                       pendingHistHeight != wantHistHeight) {
                pendingColumns = wantColumns;
                pendingImageHeight = wantHeight;
                pendingVectorscope = wantVectorscope;
                pendingHistWidth = wantHistWidth;
                pendingHistHeight = wantHistHeight;
                detailPendingSince = glfwGetTime();
            } else if (glfwGetTime() - detailPendingSince > 0.4) {
                analysis.waveform.columns = wantColumns;
                analysis.waveform.imageHeight = wantHeight;
                analysis.vectorscope.size = wantVectorscope;
                analysis.histogram.imageWidth = wantHistWidth;
                analysis.histogram.imageHeight = wantHistHeight;
                analysisDirty = true;
            }
        }

        // --- frame ---
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        ImGui::Begin("##host", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);

        // Toolbar: scope toggles, region tools, cursor readout. Switching
        // is the common case, so a plain click or key shows one scope
        // alone; Shift stacks and unstacks, and the last scope on refuses
        // to turn off, so the window never goes empty.
        // A scope draws the same frame it turns on, but the worker only
        // computes what is enabled, so a newly shown scope's image is
        // stale - from whenever the scope was last on, of whatever was on
        // screen then. Turning a scope on therefore pushes the settings
        // immediately (the update nudges the worker awake) and waits
        // briefly for the recompute, so the scope's first drawn frame is
        // already current. The wait is bounded; on timeout the stale
        // image stands in until the recompute lands a frame later.
        const auto imageFor = [&](ScopeGlyph kind) -> const ScopeImage& {
            switch (kind) {
            case ScopeGlyph::Vectorscope:
                return output.vectorscopeImage;
            case ScopeGlyph::Waveform:
                return output.waveformImage;
            case ScopeGlyph::WaveformParade:
                return output.waveformParadeImage;
            default:
                return output.histogramImage;
            }
        };
        const auto uploadVisibleScopes = [&] {
            if (view.shows(ScopeGlyph::Vectorscope)) {
                uploadScope(vectorscopeTexture, output.vectorscopeImage);
            }
            if (view.shows(ScopeGlyph::Waveform)) {
                uploadScope(waveformTexture, output.waveformImage);
            }
            if (view.shows(ScopeGlyph::WaveformParade)) {
                uploadScope(waveformParadeTexture, output.waveformParadeImage);
            }
            if (view.shows(ScopeGlyph::Histogram)) {
                uploadScope(histogramTexture, output.histogramImage);
            }
        };
        const auto refreshActivatedScope = [&](ScopeGlyph kind) {
            if (kind == ScopeGlyph::ColorPicker) {
                return;
            }
            const uint64_t staleSequence = imageFor(kind).sequence;
            worker.updateSettings(analysis);
            const double deadline = glfwGetTime() + 0.08;
            while (glfwGetTime() < deadline) {
                if (worker.fetchOutput(outputVersion, output) && imageFor(kind).sequence != staleSequence &&
                    imageFor(kind).width > 0) {
                    uploadVisibleScopes();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            uploadVisibleScopes();  // timeout: a stale image beats none
        };
        const auto toggleScope = [&](ScopeGlyph kind) {
            const bool activated = view.toggle(kind);
            analysis.enabledScopes = view.enabledMask();
            if (activated) {
                refreshActivatedScope(kind);
            }
            analysisDirty = true;
        };
        const auto chooseScope = [&](ScopeGlyph kind, bool stack) {
            const bool activated = view.choose(kind, stack);
            analysis.enabledScopes = view.enabledMask();
            if (activated) {
                refreshActivatedScope(kind);
            }
            analysisDirty = true;
        };
        // The stacking modifier reads the OS's live key state, not the
        // event-tracked one: ImGui's backend re-injects GLFW's cached
        // modifiers on every key press and click, so a Shift key-up
        // swallowed by a system overlay (the screenshot interface) leaves
        // the cache stuck exactly when the user next switches a scope.
        const ModifierState modifiers = currentModifiers();
        const bool stackModifier = modifiers.shift;
        // Command, Control, and Option chords belong to the system and
        // the window - Cmd+W closes a window on macOS, it must never
        // open the waveform - so any of them silences the plain-letter
        // shortcuts. Shift alone stays meaningful: it stacks.
        const bool systemChord = modifiers.command || modifiers.control || modifiers.option;
        const auto scopeToggle = [&](const char* id, ScopeGlyph kind, const char* tooltip) {
            const char letter[2] = {scopeLetter(kind), '\0'};
            if (scopeToggleButton(id, letter, view.shows(kind), tooltip)) {
                chooseScope(kind, stackModifier);
            }
            ImGui::SameLine(0.0f, 2.0f);
        };
        // Tooltips name the configured shortcut, not an assumed one.
        char tooltip[96];
        const auto scopeTooltip = [&](const char* name, const std::string& binding, const char* extra) {
            std::snprintf(tooltip, sizeof(tooltip), "%s - %s to switch, Shift+%s to stack%s", name, binding.c_str(),
                          binding.c_str(), extra);
            return tooltip;
        };
        scopeToggle("##toggle-vectorscope", ScopeGlyph::Vectorscope,
                    scopeTooltip("Vectorscope", shortcuts.vectorscope, ""));
        scopeToggle("##toggle-waveform", ScopeGlyph::Waveform,
                    scopeTooltip("Waveform", shortcuts.waveform, "; styles in the right-click menu"));
        scopeToggle("##toggle-waveform-parade", ScopeGlyph::WaveformParade,
                    scopeTooltip("RGB parade", shortcuts.parade, ""));
        scopeToggle("##toggle-histogram", ScopeGlyph::Histogram, scopeTooltip("Histogram", shortcuts.histogram, ""));
        scopeToggle("##toggle-color-picker", ScopeGlyph::ColorPicker,
                    scopeTooltip("Color picker", shortcuts.colorPicker, ""));
        ImGui::SameLine(0.0f, 8.0f);

        // Keyboard shortcuts mirror the toolbar and region tools.
        std::optional<RegionPickerMode> wantRegionPick;
        // Pins mark the vectorscope and the color picker; without either
        // on screen, the tool's button, menu entries, and shortcuts all
        // stand down together.
        const bool pinsAvailable = view.shows(ScopeGlyph::Vectorscope) || view.shows(ScopeGlyph::ColorPicker);
        const auto pressed = [&](const std::string& binding) {
            const ImGuiKey key = keyFor(binding);
            return key != ImGuiKey_None && ImGui::IsKeyPressed(key, false);
        };
        if (platformHidesWindowOnCommandW() && modifiers.command && !modifiers.control && !modifiers.option &&
            !io.WantTextInput) {
            // Cmd+W dismisses through the system hide - the exact
            // machinery behind Cmd+H, so the Dock click or Cmd+Tab
            // restores every window natively, the border included.
            // Hiding only the GLFW window stranded the application with
            // no visible way back.
            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
                hideApplication();
            }
            // Cmd+comma opens settings everywhere on macOS.
            if (ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
                showSettings = true;
            }
        }
        if (platformMinimizesWindowOnControlW() && modifiers.control && !modifiers.command && !modifiers.option &&
            !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            glfwIconifyWindow(window);
        }
        if (platformQuitsOnControlQ() && modifiers.control && !modifiers.command && !modifiers.option &&
            !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        if (!io.WantTextInput && !systemChord) {
            if (pressed(shortcuts.vectorscope)) {
                chooseScope(ScopeGlyph::Vectorscope, stackModifier);
            }
            if (pressed(shortcuts.waveform)) {
                chooseScope(ScopeGlyph::Waveform, stackModifier);
            }
            if (pressed(shortcuts.parade)) {
                chooseScope(ScopeGlyph::WaveformParade, stackModifier);
            }
            if (pressed(shortcuts.histogram)) {
                chooseScope(ScopeGlyph::Histogram, stackModifier);
            }
            if (pressed(shortcuts.colorPicker)) {
                chooseScope(ScopeGlyph::ColorPicker, stackModifier);
            }
            if (pressed(shortcuts.pickWindow)) {
                wantRegionPick = RegionPickerMode::PickWindows;
            }
            if (pressed(shortcuts.drawRegion)) {
                wantRegionPick = RegionPickerMode::Draw;
            }
            if (supportsFaceDetection() && pressed(shortcuts.pickFaces)) {
                wantRegionPick = RegionPickerMode::PickFaces;
            }
            if (pinsAvailable && pressed(shortcuts.pinColor)) {
                // One pin tool; each click inside decides between
                // pin-and-close and Shift's pin-and-continue. A leftover
                // Shift on the shortcut itself changes nothing.
                wantRegionPick = RegionPickerMode::PinColor;
            }
            if (pressed(shortcuts.vectorscopeZoom)) {
                view.setZoom(view.zoom() >= 4 ? 1 : view.zoom() * 2);
            }
            if (pressed(shortcuts.fullRegion)) {
                // Escape peels back one layer at a time: the settings
                // window first, the drawn region only when nothing is
                // stacked above it.
                if (showSettings) {
                    showSettings = false;
                } else {
                    resetRegionToFull();
                }
            }
        }

        std::snprintf(tooltip, sizeof(tooltip), "Draw an area (%s)", shortcuts.drawRegion.c_str());
        if (iconButton("##draw-region", RegionIcon::Crosshair, tooltip)) {
            wantRegionPick = RegionPickerMode::Draw;
        }
        ImGui::SameLine(0.0f, 2.0f);
        std::snprintf(tooltip, sizeof(tooltip), "Pick a window (%s)", shortcuts.pickWindow.c_str());
        if (iconButton("##pick-region", RegionIcon::PickHand, tooltip)) {
            wantRegionPick = RegionPickerMode::PickWindows;
        }
        ImGui::SameLine(0.0f, 2.0f);
        if (pinsAvailable) {
            std::snprintf(tooltip, sizeof(tooltip), "Pin a color (%s) - Shift+click a color to pin several",
                          shortcuts.pinColor.c_str());
            if (iconButton("##pin-color", RegionIcon::Dropper, tooltip)) {
                wantRegionPick = RegionPickerMode::PinColor;
            }
            ImGui::SameLine(0.0f, 2.0f);
        }
        // The face button sits last among the pickers: it is the one
        // most often dimmed, and a disabled button reads best at the
        // row's edge.
        if (supportsFaceDetection()) {
            const bool noneFound = g_facesOnScreen.load() == 0;
            std::snprintf(tooltip, sizeof(tooltip), "Pick a face (%s)%s", shortcuts.pickFaces.c_str(),
                          noneFound ? " - none on screen right now" : "");
            if (iconButton("##pick-face", RegionIcon::Face, tooltip, noneFound)) {
                wantRegionPick = RegionPickerMode::PickFaces;
            }
            ImGui::SameLine(0.0f, 2.0f);
        }
        if (!isFullRegion()) {
            if (iconButton("##full-region", RegionIcon::Expand, "Reset to full screen (Esc)")) {
                resetRegionToFull();
            }
            ImGui::SameLine(0.0f, 2.0f);
        }

        // The readout yields before the icons do: on a narrow window it
        // would right-align on top of the toolbar buttons, and whoever
        // wants the window that small still needs the buttons - the
        // cursor color has the vectorscope marker and the context menu.
        const float toolbarEnd = ImGui::GetCursorPosX();
        if (vectorscopeColor) {
            const FloatColor& color = *vectorscopeColor;
            // The readout's geometry never follows its digits: each value
            // gets a column sized for the widest it can be and is
            // right-aligned inside it, so neither the swatch nor the
            // numbers wander as the cursor moves across the screen.
            const float columnWidth = ImGui::CalcTextSize("100%").x;
            const float columnGap = ImGui::CalcTextSize(" ").x;
            const float swatch = ImGui::GetTextLineHeight();
            const float textWidth = 3 * columnWidth + 2 * columnGap;
            const float readoutStart = ImGui::GetWindowContentRegionMax().x - (textWidth + swatch + 6);
            if (readoutStart >= toolbarEnd + 8) {
                ImGui::SameLine(readoutStart);
                ImGui::ColorButton("##cursor-color", ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                   ImVec2(swatch, swatch));
                const float columnsStart = readoutStart + swatch + 6;
                const float channels[3] = {color.r, color.g, color.b};
                for (int channel = 0; channel < 3; ++channel) {
                    char value[8];
                    std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55);
                    const float columnStart = columnsStart + channel * (columnWidth + columnGap);
                    ImGui::SameLine(columnStart + columnWidth - ImGui::CalcTextSize(value).x);
                    ImGui::TextUnformatted(value);
                }
            } else {
                ImGui::NewLine();
            }
        } else {
            ImGui::NewLine();
        }

        const auto drawVectorscope = [&] {
            const DrawnScope scope = drawScopeImage(*vectorscopeTexture, true, static_cast<float>(view.zoom()));
            if (const auto adjusted =
                    traceIntensityGesture(scope, TraceControl::Vectorscope, view.intensity(TraceControl::Vectorscope),
                                          3.0f, VectorscopeIntensityShift, flash)) {
                view.setIntensity(TraceControl::Vectorscope, adjusted->intensity);
                analysis.vectorscope.gain = adjusted->gain;
                analysisDirty = true;
            }
            // Zoomed overlays run past the pane; clip them to it.
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(scope.origin, ImVec2(scope.origin.x + scope.size.x, scope.origin.y + scope.size.y),
                               true);
            if (view.graticule()) {
                drawVectorscopeOverlay(scope, buildVectorscopeGraticule(projectionVectorscope));
            }
            for (const FloatColor& pinned : pins.colors()) {
                if (const auto point = projectionVectorscope.project(pinned)) {
                    drawPointMarker(scope, *point, IM_COL32(230, 170, 90, 230));
                }
            }
            if (vectorscopeColor) {
                if (const auto point = projectionVectorscope.project(*vectorscopeColor)) {
                    drawPointMarker(scope, *point, IM_COL32(255, 255, 255, 255));
                }
            }
            draw->PopClipRect();
            if (view.zoom() > 1) {
                char badge[4] = {static_cast<char>('0' + view.zoom()), 'x', '\0'};
                draw->AddText(ImVec2(scope.origin.x + scope.size.x - 26, scope.origin.y + 6), GraticuleLabel, badge);
            }
        };
        // The waveform flavors share gain and graticule; the cursor
        // markers differ - channel levels on RGB and parade, one luma
        // level on luma.
        const auto drawChannelMarkers = [&](const DrawnScope& scope) {
            if (!waveformColor) {
                return;
            }
            ChannelMarker markers[3];
            const int count = groupChannelMarkers(*waveformColor, markers);
            for (int i = 0; i < count; ++i) {
                drawLevelMarker(scope, (255.0f - markers[i].value) / 255.0f, markers[i].color);
            }
        };
        // The parade separates the channels into thirds, so each marker
        // stays a single color inside its own channel's column.
        const auto drawParadeMarkers = [&](const DrawnScope& scope) {
            if (!waveformColor) {
                return;
            }
            const float channels[3] = {waveformColor->r, waveformColor->g, waveformColor->b};
            for (int channel = 0; channel < 3; ++channel) {
                drawLevelMarker(scope, (255.0f - channels[channel]) / 255.0f, channelMaskColor(1 << channel),
                                channel / 3.0f, (channel + 1) / 3.0f);
            }
        };
        const auto drawWaveform = [&](ScopeGlyph kind) {
            const ScopeTexture& texture = kind == ScopeGlyph::Waveform ? *waveformTexture : *waveformParadeTexture;
            const DrawnScope scope = drawScopeImage(texture, false);
            if (const auto adjusted = traceIntensityGesture(
                    scope, TraceControl::Waveform, view.intensity(TraceControl::Waveform), 0.05f, 0.0f, flash)) {
                view.setIntensity(TraceControl::Waveform, adjusted->intensity);
                analysis.waveform.gain = adjusted->gain;
                analysisDirty = true;
            }
            if (view.graticule()) {
                drawWaveformOverlay(scope);
            }
            if (kind == ScopeGlyph::Waveform &&
                (analysis.waveform.mode == WaveformMode::Luma || analysis.waveform.mode == WaveformMode::ColoredLuma)) {
                if (waveformColor) {
                    if (const auto point = projectionWaveform.project(*waveformColor)) {
                        drawLevelMarker(scope, point->y, IM_COL32(255, 220, 80, 220));
                    }
                }
            } else if (kind == ScopeGlyph::Waveform) {
                drawChannelMarkers(scope);
            } else {
                drawParadeMarkers(scope);
            }
        };

        // The enabled scopes stack in a fixed order, splitting the window
        // along its longer axis.
        // Which pane is under the cursor decides which options the
        // context menu shows; rects refresh as the panes draw.
        ImVec4 paneRects[5] = {};
        const auto drawScope = [&](ScopeGlyph kind) {
            panePoints[static_cast<int>(kind)] = ImGui::GetContentRegionAvail();
            const ImVec2 paneMin = ImGui::GetCursorScreenPos();
            const ImVec2 paneAvail = ImGui::GetContentRegionAvail();
            paneRects[static_cast<int>(kind)] =
                ImVec4(paneMin.x, paneMin.y, paneMin.x + paneAvail.x, paneMin.y + paneAvail.y);
            if (kind == ScopeGlyph::Vectorscope) {
                drawVectorscope();
            } else if (kind == ScopeGlyph::Histogram) {
                drawHistogram(*histogramTexture, output, analysis.histogram.style, view.graticule(), vectorscopeColor);
            } else if (kind == ScopeGlyph::ColorPicker) {
                drawColorPicker(vectorscopeColor, pins);
            } else {
                drawWaveform(kind);
            }
        };
        const int paneCount = static_cast<int>(view.stack().size());
        const ImVec2 area = ImGui::GetContentRegionAvail();
        if (!captureController.permissionGranted()) {
            drawCaptureHelp("SideScopes cannot see the screen",
                            {
                                "macOS requires the Screen Recording permission.",
                                "",
                                "1. Click the button below",
                                "2. Turn on SideScopes in the list",
                                "3. Quit and reopen SideScopes",
                            },
                            true);
        } else if (captureController.dead()) {
            const std::string status = captureController.status();
            drawCaptureHelp("Screen capture was interrupted", {status, "Reconnecting automatically..."}, false);
        } else if (paneCount <= 1) {
            if (paneCount == 1) {
                drawScope(view.stack().front());
            }
        } else {
            const bool horizontal = area.x >= area.y;
            const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
            const ImVec2 paneSize = horizontal ? ImVec2((area.x - spacing.x * (paneCount - 1)) / paneCount, area.y)
                                               : ImVec2(area.x, (area.y - spacing.y * (paneCount - 1)) / paneCount);
            const char* paneIds[5] = {"##pane0", "##pane1", "##pane2", "##pane3", "##pane4"};
            for (int pane = 0; pane < paneCount; ++pane) {
                ImGui::BeginChild(paneIds[pane], paneSize);
                drawScope(view.stack()[static_cast<std::size_t>(pane)]);
                ImGui::EndChild();
                if (horizontal && pane + 1 < paneCount) {
                    ImGui::SameLine();
                }
            }
        }

        // Right-click: the native menu carries the modes and toggles.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            using Kind = NativeMenuItem::Kind;
            // One rule shapes the menu: ownership shows through position
            // and grouping, never through label prefixes. A right-click
            // on a pane puts that scope's options first, unprefixed - the
            // click itself is the context - while a click on the toolbar
            // or background wraps each visible scope's options in a
            // submenu named after it. Every other section keeps the same
            // order in both: scopes, region, pins, view, application.
            const ImVec2 mouse = ImGui::GetMousePos();
            int clickedPane = -1;
            for (int pane = 0; pane < 5; ++pane) {
                const ImVec4& rect = paneRects[pane];
                if (rect.z <= rect.x || rect.w <= rect.y) {
                    continue;
                }
                if (mouse.x >= rect.x && mouse.x < rect.z && mouse.y >= rect.y && mouse.y < rect.w) {
                    clickedPane = pane;
                }
            }
            const auto clicked = [&](ScopeGlyph kind) { return clickedPane == static_cast<int>(kind); };
            const auto shownOrGlobal = [&](ScopeGlyph kind) { return clickedPane < 0 && view.shows(kind); };

            std::vector<NativeMenuItem> menu;
            const auto action = [&](const char* label, int id, bool checked, std::string shortcut = "") {
                menu.push_back({Kind::Action, label, id, checked, std::move(shortcut)});
            };
            const auto separator = [&] { menu.push_back({Kind::Separator, "", -1, false, ""}); };
            const auto submenu = [&](const char* label) { menu.push_back({Kind::SubmenuBegin, label, -1, false, ""}); };
            const auto endSubmenu = [&] { menu.push_back({Kind::SubmenuEnd, "", -1, false, ""}); };

            // Pins are a scope tool: they mark the vectorscope and the
            // color picker, so their submenu rides those scopes' own
            // sections and never appears beside a waveform.
            const auto pinOptions = [&] {
                submenu("Pins");
                action("Pin Colors...", MenuPickPinColor, false, shortcutLabel(shortcuts.pinColor));
                if (!pins.empty()) {
                    action("Clear Pinned Markers", MenuClearPinnedMarkers, false);
                }
                endSubmenu();
            };
            const auto vectorscopeOptions = [&] {
                submenu("Matrix");
                const bool bt601 = analysis.vectorscope.matrix == ChromaMatrix::Bt601;
                action("BT.601", MenuMatrixBt601, bt601);
                action("BT.709", MenuMatrixBt709, !bt601);
                endSubmenu();
                submenu("Trace Response");
                action("Boosted", MenuTraceBoosted, analysis.vectorscope.response == TraceResponse::Boosted);
                action("Linear", MenuTraceLinear, analysis.vectorscope.response == TraceResponse::Linear);
                endSubmenu();
                submenu("Zoom");
                action("1x", MenuZoom1, view.zoom() == 1, shortcutLabel(shortcuts.vectorscopeZoom));
                action("2x", MenuZoom2, view.zoom() == 2, shortcutLabel(shortcuts.vectorscopeZoom));
                action("4x", MenuZoom4, view.zoom() == 4, shortcutLabel(shortcuts.vectorscopeZoom));
                endSubmenu();
                pinOptions();
            };
            const auto waveformOptions = [&] {
                action("RGB", MenuWaveformStyleRgb, analysis.waveform.mode == WaveformMode::Rgb);
                action("Luma", MenuWaveformStyleLuma, analysis.waveform.mode == WaveformMode::Luma);
                action("Luma (Colored)", MenuWaveformStyleColoredLuma,
                       analysis.waveform.mode == WaveformMode::ColoredLuma);
            };
            const auto histogramOptions = [&] {
                action("Per Channel", MenuHistogramPerChannel, analysis.histogram.style == HistogramStyle::PerChannel);
                action("Combined", MenuHistogramCombined, analysis.histogram.style == HistogramStyle::Combined);
            };

            // The clicked pane's options, first and unprefixed.
            if (clicked(ScopeGlyph::Vectorscope)) {
                vectorscopeOptions();
                separator();
            } else if (clicked(ScopeGlyph::Waveform)) {
                submenu("Style");
                waveformOptions();
                endSubmenu();
                separator();
            } else if (clicked(ScopeGlyph::Histogram)) {
                submenu("Style");
                histogramOptions();
                endSubmenu();
                separator();
            } else if (clicked(ScopeGlyph::ColorPicker)) {
                pinOptions();
                separator();
            }

            submenu("Scopes");
            action("Vectorscope", MenuShowVectorscope, view.shows(ScopeGlyph::Vectorscope),
                   shortcutLabel(shortcuts.vectorscope));
            action("Waveform", MenuShowWaveform, view.shows(ScopeGlyph::Waveform), shortcutLabel(shortcuts.waveform));
            action("RGB Parade", MenuShowWaveformParade, view.shows(ScopeGlyph::WaveformParade),
                   shortcutLabel(shortcuts.parade));
            action("Histogram", MenuShowHistogram, view.shows(ScopeGlyph::Histogram),
                   shortcutLabel(shortcuts.histogram));
            action("Color Picker", MenuShowColorPicker, view.shows(ScopeGlyph::ColorPicker),
                   shortcutLabel(shortcuts.colorPicker));
            endSubmenu();

            // On a global click, each visible scope's options ride under
            // the scope's own name.
            if (shownOrGlobal(ScopeGlyph::Vectorscope)) {
                submenu("Vectorscope");
                vectorscopeOptions();
                endSubmenu();
            }
            if (shownOrGlobal(ScopeGlyph::Waveform)) {
                submenu("Waveform");
                waveformOptions();
                endSubmenu();
            }
            if (shownOrGlobal(ScopeGlyph::Histogram)) {
                submenu("Histogram");
                histogramOptions();
                endSubmenu();
            }
            // The vectorscope's section already carries the pins; when
            // only the color picker is up, they ride under its name.
            if (shownOrGlobal(ScopeGlyph::ColorPicker) && !view.shows(ScopeGlyph::Vectorscope)) {
                submenu("Color Picker");
                pinOptions();
                endSubmenu();
            }

            separator();
            action("Pick Window or Photo...", MenuSelectRegion, false, shortcutLabel(shortcuts.pickWindow));
            action("Draw Area...", MenuDrawRegion, false, shortcutLabel(shortcuts.drawRegion));
            if (supportsFaceDetection()) {
                action("Find Faces...", MenuPickFaces, false, shortcutLabel(shortcuts.pickFaces));
            }
            action("Watch Full Screen", MenuFullScreenRegion, isFullRegion(), shortcutLabel(shortcuts.fullRegion));

            separator();
            action("Graticule", MenuToggleGraticule, view.graticule());

            separator();
            action("Settings...", MenuOpenSettings, false);
            action("Quit", MenuQuit, false);

            switch (showNativeContextMenu(menu)) {
            case MenuShowVectorscope:
                toggleScope(ScopeGlyph::Vectorscope);
                break;
            case MenuShowWaveform:
                toggleScope(ScopeGlyph::Waveform);
                break;
            case MenuShowWaveformParade:
                toggleScope(ScopeGlyph::WaveformParade);
                break;
            case MenuWaveformStyleRgb:
                analysis.waveform.mode = WaveformMode::Rgb;
                analysisDirty = true;
                break;
            case MenuWaveformStyleLuma:
                analysis.waveform.mode = WaveformMode::Luma;
                analysisDirty = true;
                break;
            case MenuWaveformStyleColoredLuma:
                analysis.waveform.mode = WaveformMode::ColoredLuma;
                analysisDirty = true;
                break;
            case MenuHistogramCombined:
                analysis.histogram.style = HistogramStyle::Combined;
                analysisDirty = true;
                break;
            case MenuHistogramPerChannel:
                analysis.histogram.style = HistogramStyle::PerChannel;
                analysisDirty = true;
                break;
            case MenuShowHistogram:
                toggleScope(ScopeGlyph::Histogram);
                break;
            case MenuShowColorPicker:
                toggleScope(ScopeGlyph::ColorPicker);
                break;
            case MenuMatrixBt601:
                analysis.vectorscope.matrix = ChromaMatrix::Bt601;
                analysisDirty = true;
                break;
            case MenuMatrixBt709:
                analysis.vectorscope.matrix = ChromaMatrix::Bt709;
                analysisDirty = true;
                break;
            case MenuTraceBoosted:
                analysis.vectorscope.response = TraceResponse::Boosted;
                analysisDirty = true;
                break;
            case MenuTraceLinear:
                analysis.vectorscope.response = TraceResponse::Linear;
                analysisDirty = true;
                break;
            case MenuSelectRegion:
                wantRegionPick = RegionPickerMode::PickWindows;
                break;
            case MenuDrawRegion:
                wantRegionPick = RegionPickerMode::Draw;
                break;
            case MenuPickFaces:
                wantRegionPick = RegionPickerMode::PickFaces;
                break;
            case MenuZoom1:
                view.setZoom(1);
                break;
            case MenuZoom2:
                view.setZoom(2);
                break;
            case MenuZoom4:
                view.setZoom(4);
                break;
            case MenuFullScreenRegion:
                resetRegionToFull();
                break;
            case MenuToggleGraticule:
                view.setGraticule(!view.graticule());
                break;
            case MenuPickPinColor:
                wantRegionPick = RegionPickerMode::PinColor;
                break;
            case MenuClearPinnedMarkers:
                pins.clear();
                break;
            case MenuOpenSettings:
                showSettings = true;
                break;
            case MenuQuit:
                glfwSetWindowShouldClose(window, GLFW_TRUE);
                break;
            default:
                break;
            }
            lastActivity = glfwGetTime();
            nextPreferencesSave = glfwGetTime() + 1.0;
        }

        ImGui::End();
        ImGui::PopStyleVar();

        if (showSettings) {
            ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
            ImGui::Begin("Settings", &showSettings, ImGuiWindowFlags_NoCollapse);
            ImGui::TextWrapped("capture: %s", captureController.status().c_str());
            ImGui::Text("analysis %.2f ms | frames %llu | ui %.0f fps", output.accumulateMilliseconds,
                        static_cast<unsigned long long>(output.framesProcessed), static_cast<double>(io.Framerate));
            ImGui::Separator();
            ImGui::TextDisabled("vectorscope");
            float vectorscopePercent = view.intensity(TraceControl::Vectorscope);
            if (ImGui::SliderFloat("intensity##v", &vectorscopePercent, 0.0f, 100.0f, "%.0f%%")) {
                view.setIntensity(TraceControl::Vectorscope, vectorscopePercent);
                analysis.vectorscope.gain = traceGainFromIntensity(vectorscopePercent, VectorscopeIntensityShift);
                analysisDirty = true;
            }
            analysisDirty |= ImGui::SliderInt("sampling 1:N##v", &analysis.vectorscope.samplingStride, 1, 8);
            float vectorscopeMs = view.smoothing(TraceControl::Vectorscope);
            if (ImGui::SliderFloat("smoothing ms##v", &vectorscopeMs, 0.0f, 500.0f, "%.0f")) {
                view.setSmoothing(TraceControl::Vectorscope, vectorscopeMs);
            }
            ImGui::TextDisabled("waveform");
            float waveformPercent = view.intensity(TraceControl::Waveform);
            if (ImGui::SliderFloat("intensity##w", &waveformPercent, 0.0f, 100.0f, "%.0f%%")) {
                view.setIntensity(TraceControl::Waveform, waveformPercent);
                analysis.waveform.gain = traceGainFromIntensity(waveformPercent);
                analysisDirty = true;
            }
            analysisDirty |= ImGui::SliderInt("sampling 1:N##w", &analysis.waveform.samplingStride, 1, 8);
            float waveformMs = view.smoothing(TraceControl::Waveform);
            if (ImGui::SliderFloat("smoothing ms##w", &waveformMs, 0.0f, 500.0f, "%.0f")) {
                view.setSmoothing(TraceControl::Waveform, waveformMs);
            }
            ImGui::TextDisabled("modes and toggles: right-click a scope");
            ImGui::TextDisabled("%s", versionInfo.display.c_str());
            ImGui::End();
        }

        if (ImGui::IsAnyItemActive()) {
            lastActivity = glfwGetTime();
            nextPreferencesSave = glfwGetTime() + 1.0;
        }

        ImGui::Render();
        graphics->endFrame();

        // The blocking overlay runs after the frame is submitted; capture and
        // analysis keep flowing underneath.
        if (regionPicking && wantRegionPick) {
            const bool targetPin = *wantRegionPick == RegionPickerMode::PinColor;
            if (targetPin || regionPickIsPin) {
                // Region picking and color pinning are separate tools; a
                // pick never morphs across that boundary. The active pick
                // closes - with no effect on the region - and the
                // requested tool opens fresh once it is gone.
                regionPickSwallowCancel = true;
                cancelRegionPick();
            } else {
                // The toolbar keeps working mid-pick: choosing a region
                // tool switches the active picker's mode instead of
                // stacking one.
                setRegionPickMode(*wantRegionPick);
                wantRegionPick.reset();
            }
        }
        if (!regionPicking && wantRegionPick && captureController.capturedDisplay() != 0) {
            hideRegionBorder();
            // The previous region's border must not leak into the analyzed
            // frame: its strokes read as rectangle edges and cut suggestions
            // short at the old region. The latest captured frame may predate
            // the hide, so wait briefly for one taken after the border left
            // the screen. The 60 ms floor outlasts an in-flight pre-hide
            // frame's capture-to-delivery; the 300 ms cap keeps the picker
            // responsive if the capture stream has stalled.
            if (!isFullRegion()) {
                uint64_t staleSequence = 0;
                worker.withLatestFrame([&](const FrameView& view) { staleSequence = view.sequence; });
                const double hiddenAt = glfwGetTime();
                for (;;) {
                    const double elapsed = glfwGetTime() - hiddenAt;
                    if (elapsed >= 0.3) {
                        break;
                    }
                    uint64_t sequence = staleSequence;
                    worker.withLatestFrame([&](const FrameView& view) { sequence = view.sequence; });
                    // Inequality, not greater-than: a freshly switched
                    // stream counts its frames from one again.
                    if (sequence != staleSequence && elapsed >= 0.06) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                }
            }
            // The offer, per display: the visible application windows,
            // frontmost first - window rectangles come from the operating
            // system and are exact - plus, behind their own key, the faces
            // the platform detector finds in the current frame. Faces are
            // offered on the tracked display only: that is the display
            // whose pixels we hold.

            std::vector<SuggestedRegion> faceSuggestions;
            if (supportsFaceDetection()) {
                worker.withLatestFrame([&](const FrameView& view) {
                    const auto geometry = geometryOfDisplay(captureController.capturedDisplay());
                    const float pixelsPerPoint =
                        geometry ? static_cast<float>(view.width / geometry->widthPoints) : 1.0f;
                    faceSuggestions = buildFaceSuggestions(detectFaces(view, pixelsPerPoint), view.width, view.height);
                    g_facesOnScreen.store(static_cast<int>(faceSuggestions.size()));
                });
            }

            std::vector<PickerDisplay> pickerDisplays;
            for (const CaptureTarget& target : capture->listTargets()) {
                PickerDisplay entry;
                entry.displayId = target.displayId;
                entry.windows = windowSuggestionsFor(target.displayId);
                if (target.displayId == captureController.capturedDisplay()) {
                    entry.faces = faceSuggestions;
                }
                pickerDisplays.push_back(std::move(entry));
            }

            // Field diagnosis: dump exactly what the pipeline saw. Enable
            // with `launchctl setenv SIDESCOPES_DEBUG_SUGGESTIONS 1`.
            if (debugSuggestionsRequested()) {
                std::FILE* report = openDebugFile("/tmp/sidescopes-suggestions.txt", "w");
                if (report) {
                    for (const auto& entry : pickerDisplays) {
                        for (const auto& suggestion : entry.windows) {
                            std::fprintf(report, "display %u suggestion '%s' %.1f,%.1f..%.1f,%.1f%%\n", entry.displayId,
                                         suggestion.label.c_str(), suggestion.region.leftPercent,
                                         suggestion.region.topPercent, suggestion.region.rightPercent,
                                         suggestion.region.bottomPercent);
                        }
                    }
                    std::fclose(report);
                }
                worker.withLatestFrame([&](const FrameView& view) {
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
            if (beginRegionPick(pickerDisplays, *wantRegionPick)) {
                regionPicking = true;
                regionPickIsPin = *wantRegionPick == RegionPickerMode::PinColor;
            }
            // Consumed either way: a request that could not open (a
            // display gone mid-flight) must not retry every frame.
            wantRegionPick.reset();
            lastActivity = glfwGetTime();
        }

        // The region border is live: dragging its edges, corners, or move
        // tab adjusts the region with the scopes following along.
        if (!regionPicking) {
            const RegionBorderEdit edit = pollRegionBorderEdit();
            if (edit.dismissed) {
                // The border's own close affordances mean "stop tracking
                // this region"; full screen is the fallback.
                resetRegionToFull();
                lastActivity = glfwGetTime();
            } else if (edit.region) {
                analysis.region = *edit.region;
                // The analysis-dirty path below syncs the border this same
                // iteration; a second sync here would double the border
                // work on every drag step.
                analysisDirty = true;
                lastActivity = glfwGetTime();
            }
        }

        // While the picker is up, whatever the user indicates previews on
        // the scopes immediately; confirmation keeps it, Esc restores.
        if (regionPicking) {
            const RegionPickPoll poll = pollRegionPick();
            const auto applyRegion = [&](const RegionOfInterest& region) {
                if (region.leftPercent == analysis.region.leftPercent &&
                    region.topPercent == analysis.region.topPercent &&
                    region.rightPercent == analysis.region.rightPercent &&
                    region.bottomPercent == analysis.region.bottomPercent) {
                    return;
                }
                analysis.region = region;
                analysisDirty = true;
                lastActivity = glfwGetTime();
            };
            if (poll.pinMode) {
                // Color pinning never touches the region: clicks and
                // drags deliver areas to average, and a finish - Esc, or
                // the single flavor's one pin - just puts things back.
                std::optional<FloatColor> chip;
                if (const auto cursor = globalCursorPosition()) {
                    if (displayAtPoint(*cursor).value_or(0) == captureController.capturedDisplay() &&
                        !captureController.dead()) {
                        if (const auto geometry = geometryOfDisplay(captureController.capturedDisplay())) {
                            const double spanX = PinSamplePoints * uiScale / geometry->widthPoints * 100.0;
                            const double spanY = PinSamplePoints * uiScale / geometry->heightPoints * 100.0;
                            RegionOfInterest patch;
                            patch.leftPercent =
                                (cursor->x - geometry->originX) / geometry->widthPoints * 100.0 - spanX / 2;
                            patch.rightPercent = patch.leftPercent + spanX;
                            patch.topPercent =
                                (cursor->y - geometry->originY) / geometry->heightPoints * 100.0 - spanY / 2;
                            patch.bottomPercent = patch.topPercent + spanY;
                            chip = averageFrameArea(patch);
                        }
                    } else {
                        // Another display: the throttled one-shot sampler
                        // already tracks the cursor there.
                        std::lock_guard lock(screenSample->mutex);
                        chip = screenSample->color;
                    }
                }
                setRegionPickChipColor(chip);

                if (poll.pinnedArea) {
                    std::optional<FloatColor> pinned;
                    if (poll.displayId == captureController.capturedDisplay() && !captureController.dead()) {
                        pinned = averageFrameArea(*poll.pinnedArea);
                    } else {
                        std::lock_guard lock(screenSample->mutex);
                        pinned = screenSample->color;
                    }
                    if (pinned) {
                        pins.pin(*pinned);
                    }
                    // The click's own Shift decided: pin-and-continue
                    // stays, a plain pin ends the errand.
                    if (!poll.pinnedKeepOpen) {
                        cancelRegionPick();
                    }
                    lastActivity = glfwGetTime();
                }
                if (poll.finished || !poll.active) {
                    regionPicking = false;
                    regionPickSwallowCancel = false;
                    syncRegionBorder();
                    lastActivity = glfwGetTime();
                }
            } else {
                // Live preview only for the tracked display: previewing a
                // suggestion on another display would mean flapping the
                // capture stream on every hover. The switch happens once,
                // on confirmation.
                if (poll.preview && poll.displayId == captureController.capturedDisplay()) {
                    applyRegion(*poll.preview);
                }
                if (poll.finished || !poll.active) {
                    regionPicking = false;
                    if (poll.confirmed) {
                        if (poll.displayId != 0 && poll.displayId != captureController.capturedDisplay()) {
                            captureController.requestDisplay(poll.displayId);
                            captureController.start();
                        }
                        applyRegion(*poll.confirmed);
                    } else if (!regionPickSwallowCancel) {
                        // Cancelled: reset all drawing, pending and
                        // confirmed. Full region means capture snaps back
                        // to the display this window sits on. A cancel
                        // ordered by a tool switch is not the user's Esc
                        // and resets nothing.
                        applyRegion(RegionOfInterest{});
                    }
                    regionPickSwallowCancel = false;
                    syncRegionBorder();
                    lastActivity = glfwGetTime();
                }
            }
        }

        if (analysisDirty) {
            worker.updateSettings(analysis);
            projectionVectorscope.configure(analysis.vectorscope);
            projectionWaveform.configure(analysis.waveform);
            syncRegionBorder();
            analysisDirty = false;
            lastActivity = glfwGetTime();
            nextPreferencesSave = glfwGetTime() + 1.0;
        }
        if (nextPreferencesSave > 0.0 && glfwGetTime() > nextPreferencesSave) {
            persistPreferences();
            nextPreferencesSave = -1.0;
        }
    }

    persistPreferences();
    hideRegionBorder();
    worker.stop();
    capture->stop();
    graphics->shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
