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

#include "core/analysis_worker.h"
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

namespace {

using namespace sidescopes;

enum MenuAction {
    kMenuShowVectorscope = 1,
    kMenuShowWaveform,
    kMenuShowWaveformParade,
    kMenuShowHistogram,
    kMenuShowColorPicker,
    kMenuWaveformStyleRgb = 10,
    kMenuWaveformStyleLuma,
    kMenuHistogramCombined,
    kMenuHistogramPerChannel,
    kMenuMatrixBt601 = 20,
    kMenuMatrixBt709,
    kMenuTraceBoosted,
    kMenuTraceLinear,
    kMenuWaveformStyleColoredLuma,
    kMenuDrawRegion,
    kMenuPickFaces,
    kMenuZoom1,
    kMenuZoom2,
    kMenuZoom4,
    kMenuSelectRegion = 30,
    kMenuFullScreenRegion,
    kMenuToggleGraticule = 40,
    kMenuTogglePercentValues,
    kMenuPinCursorColor,
    kMenuPinRegionAverage,
    kMenuClearPinnedMarkers,
    kMenuOpenSettings = 50,
    kMenuQuit,
};

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

struct DrawnScope {
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
DrawnScope DrawScopeImage(const ScopeTexture& texture, bool keep_aspect, float zoom = 1.0f) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImVec2 size = available;
    if (keep_aspect) {
        const float scale =
            std::max(0.05f, std::min(available.x / static_cast<float>(texture.Width()),
                                     available.y / static_cast<float>(texture.Height())));
        size = ImVec2(texture.Width() * scale, texture.Height() * scale);
    }
    ImVec2 cursor = ImGui::GetCursorPos();
    cursor.x += std::max(0.0f, (available.x - size.x) * 0.5f);
    cursor.y += std::max(0.0f, (available.y - size.y) * 0.5f);
    ImGui::SetCursorPos(cursor);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float crop = 0.5f / zoom;
    ImGui::Image(texture.Id(), size, ImVec2(0.5f - crop, 0.5f - crop),
                 ImVec2(0.5f + crop, 0.5f + crop));
    return DrawnScope{origin, size, zoom};
}

ImVec2 At(const DrawnScope& scope, const NormalizedPoint& point) {
    const float x = (point.x - 0.5f) * scope.zoom + 0.5f;
    const float y = (point.y - 0.5f) * scope.zoom + 0.5f;
    return ImVec2(scope.origin.x + x * scope.size.x, scope.origin.y + y * scope.size.y);
}

// Every scope speaks with one graticule voice: the same champagne gold
// at a handful of strengths. The scales used to be neutral gray while
// the vectorscope wore gold, which read as two different instruments.
constexpr ImU32 kGraticuleMinor = IM_COL32(205, 172, 110, 70);
constexpr ImU32 kGraticuleMajor = IM_COL32(205, 172, 110, 130);
constexpr ImU32 kGraticuleAccent = IM_COL32(218, 175, 95, 180);
constexpr ImU32 kGraticuleLabel = IM_COL32(226, 198, 145, 200);
constexpr ImU32 kGraticuleSkinTone = IM_COL32(230, 170, 140, 160);

void DrawVectorscopeOverlay(const DrawnScope& scope, const VectorscopeGraticule& graticule) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto stroke_color = [](GraticuleStroke stroke) -> ImU32 {
        switch (stroke) {
            case GraticuleStroke::GridMajor:
                return kGraticuleMajor;
            case GraticuleStroke::Accent:
                return kGraticuleAccent;
            case GraticuleStroke::SkinTone:
                return kGraticuleSkinTone;
            case GraticuleStroke::Grid:
                break;
        }
        return kGraticuleMinor;
    };

    for (const GraticuleLine& line : graticule.lines) {
        draw->AddLine(At(scope, line.from), At(scope, line.to), stroke_color(line.stroke),
                      line.stroke == GraticuleStroke::GridMajor ? 1.5f : 1.0f);
    }
    for (const GraticuleCircle& circle : graticule.circles) {
        draw->AddCircle(At(scope, circle.center), circle.radius * scope.size.x * scope.zoom,
                        stroke_color(circle.stroke), 64);
    }
    for (const GraticuleTarget& target : graticule.targets) {
        const ImVec2 center = At(scope, target.center);
        const float box = target.primary ? 5.0f : 3.0f;
        draw->AddRect(ImVec2(center.x - box, center.y - box),
                      ImVec2(center.x + box, center.y + box), kGraticuleAccent);
        if (!target.label.empty())
            draw->AddText(ImVec2(center.x + 7, center.y - 7), kGraticuleLabel,
                          target.label.c_str());
    }
}

void DrawWaveformOverlay(const DrawnScope& scope) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool roomy = scope.size.y >= 140.0f;
    for (const WaveformScaleLine& line : BuildWaveformScale()) {
        const float y = scope.origin.y + line.y * scope.size.y;
        const ImU32 color = line.major ? kGraticuleMajor : kGraticuleMinor;
        draw->AddLine(ImVec2(scope.origin.x, y), ImVec2(scope.origin.x + scope.size.x, y), color);
        if (line.major || roomy)
            draw->AddText(ImVec2(scope.origin.x + 4, y + 1), kGraticuleLabel, line.label.c_str());
    }
}

void DrawPointMarker(const DrawnScope& scope, const NormalizedPoint& point, ImU32 color) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = At(scope, point);
    draw->AddCircle(center, 5.0f, color, 0, 2.0f);
    draw->AddCircle(center, 6.5f, IM_COL32(0, 0, 0, 200), 0, 1.0f);
}

void DrawLevelMarker(const DrawnScope& scope, float normalized_y, ImU32 color, float from_x = 0.0f,
                     float to_x = 1.0f) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float y = scope.origin.y + normalized_y * scope.size.y;
    draw->AddLine(ImVec2(scope.origin.x + from_x * scope.size.x, y),
                  ImVec2(scope.origin.x + to_x * scope.size.x, y), color, 1.5f);
}

void DrawValueMarker(const DrawnScope& scope, float normalized_x, ImU32 color, float from_y = 0.0f,
                     float to_y = 1.0f) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x = scope.origin.x + normalized_x * scope.size.x;
    draw->AddLine(ImVec2(x, scope.origin.y + from_y * scope.size.y),
                  ImVec2(x, scope.origin.y + to_y * scope.size.y), color, 1.5f);
}

// Cursor markers for the three channels, merged where the values
// coincide: lines folding into one must read as the mix - gray for all
// three, yellow, magenta, or cyan for a pair - not as whichever channel
// happened to draw last.
struct ChannelMarker {
    float value = 0.0f;  // 0..255
    ImU32 color = 0;
};

ImU32 ChannelMaskColor(int mask) {
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

int GroupChannelMarkers(const FloatColor& color, ChannelMarker out[3]) {
    const float channels[3] = {color.r, color.g, color.b};
    constexpr float kMergeEpsilon = 2.0f;
    bool grouped[3] = {false, false, false};
    int count = 0;
    for (int channel = 0; channel < 3; ++channel) {
        if (grouped[channel]) continue;
        int mask = 1 << channel;
        float sum = channels[channel];
        int members = 1;
        for (int other = channel + 1; other < 3; ++other) {
            if (grouped[other]) continue;
            if (std::abs(channels[other] - channels[channel]) <= kMergeEpsilon) {
                grouped[other] = true;
                mask |= 1 << other;
                sum += channels[other];
                ++members;
            }
        }
        out[count++] = ChannelMarker{sum / static_cast<float>(members), ChannelMaskColor(mask)};
    }
    return count;
}

// Toolbar icon buttons drawn with the draw list: corner brackets for region
// selection, an outline rectangle for full screen.
enum class ScopeGlyph { Vectorscope, Waveform, WaveformParade, Histogram, ColorPicker };

// Everything stackable, in the fixed toolbar order.
constexpr ScopeGlyph kAllScopes[] = {ScopeGlyph::Vectorscope, ScopeGlyph::Waveform,
                                     ScopeGlyph::WaveformParade, ScopeGlyph::Histogram,
                                     ScopeGlyph::ColorPicker};

// Letter chips and preference letters share one alphabet.
constexpr char ScopeLetter(ScopeGlyph kind) {
    switch (kind) {
        case ScopeGlyph::Vectorscope:
            return 'V';
        case ScopeGlyph::Waveform:
            return 'W';
        case ScopeGlyph::WaveformParade:
            return 'R';
        case ScopeGlyph::Histogram:
            return 'H';
        case ScopeGlyph::ColorPicker:
            return 'C';
    }
    return 'V';
}

constexpr uint32_t ScopeEnableBit(ScopeGlyph kind) {
    switch (kind) {
        case ScopeGlyph::Vectorscope:
            return kScopeVectorscope;
        case ScopeGlyph::Waveform:
            return kScopeWaveform;
        case ScopeGlyph::WaveformParade:
            return kScopeWaveformParade;
        case ScopeGlyph::Histogram:
            return kScopeHistogram;
        case ScopeGlyph::ColorPicker:
            // Reads the sampled cursor color; asks nothing of the worker.
            return 0;
    }
    return kScopeVectorscope;
}

// Scope toggles are letter chips: professional tools label scopes with
// text because no icon language exists for them, and the letters double
// as the keyboard shortcuts.
bool ScopeToggleButton(const char* id, const char* letter, bool enabled, const char* tooltip) {
    const float height = ImGui::GetTextLineHeight() + 4.0f;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(height + 8.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (enabled)
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonActive), 3.0f);
    else if (ImGui::IsItemHovered())
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    const ImU32 color = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const ImVec2 size = ImGui::CalcTextSize(letter);
    const ImVec2 at(std::floor(min.x + (max.x - min.x - size.x) / 2),
                    std::floor(min.y + (max.y - min.y - size.y) / 2));
    draw->AddText(at, color, letter);
    ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

// Region tool icons mirror the cursors of their modes: a pointing hand
// for picking a window (the pick-mode hover cursor), a crosshair
// for drawing one (the draw-mode cursor), expanding arrows for full
// screen.
enum class RegionIcon { PickHand, Crosshair, Face, Expand };

bool IconButton(const char* id, RegionIcon icon, const char* tooltip, bool dimmed = false) {
    const float height = ImGui::GetTextLineHeight() + 4.0f;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(height + 8.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered())
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    const ImVec2 center(std::floor((min.x + max.x) / 2) + 0.5f,
                        std::floor((min.y + max.y) / 2) + 0.5f);
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
            {-2.8f, 7.5f},  {-5.6f, 1.8f},  {-6.0f, 0.4f},  {-5.6f, -0.4f}, {-4.6f, -0.6f},
            {-2.7f, 0.6f},  {-2.7f, -6.6f}, {-2.2f, -7.5f}, {-1.1f, -7.5f}, {-0.6f, -6.6f},
            {-0.6f, -2.8f}, {0.1f, -3.3f},  {1.1f, -3.3f},  {1.5f, -2.6f},  {1.7f, -2.4f},
            {2.4f, -2.5f},  {2.9f, -1.8f},  {3.2f, -1.6f},  {4.0f, -1.5f},  {4.5f, -0.8f},
            {4.5f, 6.3f},   {3.9f, 7.5f},
        };
        ImVec2 points[std::size(outline)];
        for (std::size_t i = 0; i < std::size(outline); ++i)
            points[i] = ImVec2(center.x + outline[i].x, center.y + outline[i].y);
        draw->AddPolyline(points, static_cast<int>(std::size(outline)), color, ImDrawFlags_Closed,
                          1.4f);
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
    } else {
        // Two arrows expanding to opposite corners, the fullscreen idiom.
        const auto arrow = [&](ImVec2 from, ImVec2 to, float head_x, float head_y) {
            draw->AddLine(from, to, color, stroke);
            draw->AddLine(to, ImVec2(to.x + head_x * 3.5f, to.y), color, stroke);
            draw->AddLine(to, ImVec2(to.x, to.y + head_y * 3.5f), color, stroke);
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
std::atomic<int> g_faces_on_screen{-1};
std::atomic<bool> g_face_check_requested{false};
std::atomic<bool> g_face_check_running{false};

void RefreshFacePresence(AnalysisWorker& worker, uint32_t capture_display) {
    if (!SupportsFaceDetection()) return;
    if (g_face_check_running.exchange(true)) return;
    // Detection takes long enough to hitch a frame, so it runs on a copy
    // of the latest frame in a background thread.
    auto pixels = std::make_shared<std::vector<uint8_t>>();
    int width = 0;
    int height = 0;
    worker.WithLatestFrame([&](const FrameView& view) {
        width = view.width;
        height = view.height;
        pixels->resize(static_cast<std::size_t>(view.height) * view.stride_bytes);
        std::memcpy(pixels->data(), view.bgra, pixels->size());
    });
    if (width == 0 || height == 0) {
        g_face_check_running.store(false);
        return;
    }
    float pixels_per_point = 1.0f;
    if (const auto geometry = GeometryOfDisplay(capture_display))
        pixels_per_point = static_cast<float>(width / geometry->width_points);
    std::thread([pixels, width, height, pixels_per_point] {
        const FrameView view{pixels->data(), width * 4, width, height, ColorSpaceHint::Srgb, 0};
        g_faces_on_screen.store(static_cast<int>(DetectFaces(view, pixels_per_point).size()));
        g_face_check_running.store(false);
    }).detach();
}

// The secure-CRT deprecations make std::getenv and std::fopen hard errors
// under MSVC's warnings-as-errors, so the debug-dump plumbing goes through
// the annexes Microsoft accepts.
bool DebugSuggestionsRequested() {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "SIDESCOPES_DEBUG_SUGGESTIONS") != 0 || value == nullptr)
        return false;
    std::free(value);
    return true;
#else
    return std::getenv("SIDESCOPES_DEBUG_SUGGESTIONS") != nullptr;
#endif
}

std::FILE* OpenDebugFile(const char* path, const char* mode) {
#ifdef _MSC_VER
    std::FILE* file = nullptr;
    return fopen_s(&file, path, mode) == 0 ? file : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

void ApplyTheme() {
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

void LoadInterfaceFont(GLFWwindow* window) {
    float scale_x = 2.0f;
    float scale_y = 2.0f;
    glfwGetWindowContentScale(window, &scale_x, &scale_y);
    ImFontConfig config;
    config.RasterizerDensity = scale_x;
    ImGuiIO& io = ImGui::GetIO();
    for (const std::string& path : InterfaceFontFiles()) {
        if (io.Fonts->AddFontFromFileTTF(path.c_str(), 13.0f, &config)) return;
    }
}

// The interface is authored in 100%-scale units. On macOS GLFW window
// coordinates are already such units - the framebuffer alone carries the
// Retina factor - so this is 1.0; on Windows the window is sized in
// physical pixels and the monitor's content scale (1.25, 1.5, ...) says
// how many of them the interface should treat as one.
float UiScale(GLFWwindow* window) {
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    glfwGetWindowContentScale(window, &scale_x, &scale_y);
    int window_width = 0;
    int window_height = 0;
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetWindowSize(window, &window_width, &window_height);
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    const float density =
        window_width > 0 ? framebuffer_width / static_cast<float>(window_width) : 1.0f;
    return density > 0 ? scale_x / density : scale_x;
}

}  // namespace

int main() {
    if (!glfwInit()) return 1;

    const Preferences startup = LoadPreferences(PreferencesFilePath());

    std::unique_ptr<GraphicsBackend> graphics = CreateGraphicsBackend();
    graphics->SetWindowHints();
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    // On Windows the window follows its monitor's scale, so it keeps its
    // physical size when dragged between differently scaled monitors;
    // macOS ignores the hint (scaling lives in the framebuffer there).
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(startup.window_width, startup.window_height, "SideScopes",
                                          nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    if (startup.window_x >= 0) glfwSetWindowPos(window, startup.window_x, startup.window_y);
    // Installed before the ImGui backend so it chains this callback
    // instead of being replaced by it.
    glfwSetWindowFocusCallback(window, [](GLFWwindow*, int focused) {
        if (focused) g_face_check_requested.store(true);
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is ours to persist
    ImGui::StyleColorsDark();
    ApplyTheme();
    LoadInterfaceFont(window);
    float ui_scale = UiScale(window);
    const auto apply_ui_scale = [&ui_scale](float scale) {
        ui_scale = scale;
        // Scaling an already scaled style would compound, so rebuild from
        // the base theme each time.
        ApplyTheme();
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleMain = scale;
    };
    if (ui_scale != 1.0f) apply_ui_scale(ui_scale);
    if (!graphics->Init(window)) {
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // --- capture and analysis ---
    FrameMailbox mailbox;
    AnalysisWorker worker(mailbox);
    auto capture = CreateScreenCaptureSource();

    std::mutex status_mutex;
    std::string capture_status = "starting";
    std::atomic<bool> capture_dead{false};
    capture->SetStatusCallback([&](const std::string& message) {
        {
            std::lock_guard lock(status_mutex);
            capture_status = message;
        }
        capture_dead.store(true);
    });

    uint32_t capture_display = 0;
    // The display the user chose to scope, held across stream restarts.
    // Zero means "whichever the backend lists first" - the main display.
    uint32_t desired_display = 0;
    const bool permission_granted = capture->RequestPermission() == CapturePermission::Granted;
    const auto start_capture = [&]() -> bool {
        const auto targets = capture->ListTargets();
        if (targets.empty()) return false;
        const CaptureTarget* target = &targets.front();
        if (desired_display != 0) {
            const auto wanted =
                std::find_if(targets.begin(), targets.end(), [&](const CaptureTarget& candidate) {
                    return candidate.display_id == desired_display;
                });
            if (wanted == targets.end()) {
                // The chosen display is disconnected. The scopes pause on
                // the banner rather than silently jumping to another
                // screen; the retry loop resumes the same region the
                // moment the display returns.
                std::lock_guard lock(status_mutex);
                capture_status = "display disconnected - scopes resume when it returns";
                return false;
            }
            target = &*wanted;
        }
        if (!capture->Start(*target, 30, mailbox)) return false;
        capture_display = target->display_id;
        desired_display = target->display_id;
        {
            std::lock_guard lock(status_mutex);
            capture_status = "capturing " + target->description;
        }
        capture_dead.store(false);
        return true;
    };
    // The display under this window's center: full-screen capture is a
    // promise about the screen the user can see the scopes on.
    const auto display_of_window = [&]() -> std::optional<uint32_t> {
        int window_x = 0;
        int window_y = 0;
        int window_width = 0;
        int window_height = 0;
        glfwGetWindowPos(window, &window_x, &window_y);
        glfwGetWindowSize(window, &window_width, &window_height);
        return DisplayAtPoint(
            DesktopPoint{window_x + window_width / 2.0, window_y + window_height / 2.0});
    };
    if (permission_granted) {
        desired_display = display_of_window().value_or(0);
        start_capture();
    } else {
        std::lock_guard lock(status_mutex);
        capture_status =
            "screen recording permission missing - grant it in System Settings and "
            "relaunch";
    }

    // --- state, seeded from preferences ---
    AnalysisSettings analysis;
    analysis.vectorscope.gain = startup.vectorscope_gain;
    analysis.vectorscope.sampling_stride = startup.vectorscope_stride;
    analysis.vectorscope.matrix = startup.matrix;
    analysis.vectorscope.response = startup.trace_response;
    analysis.waveform.gain = startup.waveform_gain;
    analysis.waveform.sampling_stride = startup.waveform_stride;
    analysis.histogram.sampling_stride = startup.histogram_stride;
    analysis.waveform.mode = startup.waveform_mode;
    analysis.histogram.style =
        startup.histogram_per_channel ? HistogramStyle::PerChannel : HistogramStyle::Combined;
    bool analysis_dirty = true;

    float vectorscope_intensity =
        IntensityFromTraceGain(analysis.vectorscope.gain, kVectorscopeIntensityShift);
    float waveform_intensity = IntensityFromTraceGain(analysis.waveform.gain);
    float vectorscope_smoothing_ms = startup.vectorscope_smoothing_ms;
    float waveform_smoothing_ms = startup.waveform_smoothing_ms;
    // The scopes on screen, in activation order: the pane you turned on
    // last stacks after the ones already up.
    std::vector<ScopeGlyph> scope_stack;
    for (const char letter : startup.scope_stack) {
        for (const ScopeGlyph kind : kAllScopes) {
            if (ScopeLetter(kind) == letter) scope_stack.push_back(kind);
        }
    }
    if (scope_stack.empty()) scope_stack.push_back(ScopeGlyph::Vectorscope);
    const auto scope_shown = [&](ScopeGlyph kind) {
        return std::find(scope_stack.begin(), scope_stack.end(), kind) != scope_stack.end();
    };
    const auto sync_enabled_scopes = [&] {
        analysis.enabled_scopes = 0;
        for (const ScopeGlyph kind : scope_stack) analysis.enabled_scopes |= ScopeEnableBit(kind);
    };
    sync_enabled_scopes();
    bool show_graticule = startup.show_graticule;
    bool values_as_percent = startup.values_as_percent;
    int vectorscope_zoom = startup.vectorscope_zoom;
    // Shortcuts come from the preferences file: the key handler acts on
    // them and the context menu displays them, resolved once here.
    const ShortcutBindings shortcuts = startup.shortcuts;
    const auto key_for = [](const std::string& name) -> ImGuiKey {
        if (name == "Escape") return ImGuiKey_Escape;
        if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
            return static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'A'));
        return ImGuiKey_None;
    };
    const auto shortcut_label = [](const std::string& name) -> std::string {
        return name == "Escape" ? "Esc" : name;
    };
    bool show_settings = false;
    // Reference colors pinned on the vectorscope (session-scoped): pin a
    // corrected skin tone, then match the next photo against it.
    std::vector<FloatColor> pinned_colors;
    int pinned_menu_index = -1;  // swatch under the management popup
    constexpr std::size_t kMaximumPinnedColors = 8;
    // References come from two places: what the cursor reads (P), and
    // the region's average (Shift+P) - select a face, pin its skin.
    const auto pin_reference_color = [&](const std::optional<FloatColor>& color) {
        if (!color) return;
        if (pinned_colors.size() >= kMaximumPinnedColors)
            pinned_colors.erase(pinned_colors.begin());
        pinned_colors.push_back(*color);
    };

    // Live region picking: while the overlay is up, the frame loop polls it
    // and previews the indicated region on the scopes; cancelling resets
    // to the full screen.
    bool region_picking = false;

    // Projection-only engine instances kept in sync with the analysis
    // settings; they never accumulate, they only place overlays and markers.
    Vectorscope projection_vectorscope;
    Waveform projection_waveform;

    MarkerSmoother vectorscope_marker;
    MarkerSmoother waveform_marker;

    // Scope textures are drawn the same frame their scope becomes
    // visible, possibly before anything was ever uploaded - and a fresh
    // GPU texture holds whatever memory the driver recycled into it, so
    // every texture starts blanked to the scopes' black.
    const auto create_blank_texture = [&](int width, int height) {
        auto texture = graphics->CreateScopeTexture(width, height);
        ScopeImage blank;
        blank.width = width;
        blank.height = height;
        blank.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
        for (std::size_t i = 3; i < blank.rgba.size(); i += 4) blank.rgba[i] = 255;
        texture->Upload(blank);
        return texture;
    };
    std::unique_ptr<ScopeTexture> vectorscope_texture =
        create_blank_texture(Vectorscope::kSize, Vectorscope::kSize);
    std::unique_ptr<ScopeTexture> waveform_texture =
        create_blank_texture(Waveform::kColumns, Waveform::kLevels);
    std::unique_ptr<ScopeTexture> waveform_parade_texture =
        create_blank_texture(Waveform::kColumns, Waveform::kLevels);
    std::unique_ptr<ScopeTexture> histogram_texture =
        create_blank_texture(Histogram::kImageWidth, Histogram::kHeight);
    // Adaptive scope detail: panes are measured at draw time (stacking
    // splits the window, so the pane is what matters, not the window),
    // and desired resolutions are debounced so a live resize does not
    // thrash engine reallocation. The upload path recreates a texture
    // whenever its image changes dimensions.
    ImVec2 pane_points[5] = {};
    int pending_columns = 0;
    int pending_image_height = 0;
    int pending_vectorscope = 0;
    double detail_pending_since = 0.0;
    const auto upload_scope = [&](std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image) {
        if (image.width <= 0 || image.height <= 0) return;
        if (image.rgba.size() <
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4)
            return;
        if (texture->Width() != image.width || texture->Height() != image.height)
            texture = graphics->CreateScopeTexture(image.width, image.height);
        texture->Upload(image);
    };

    worker.Start();
    WarmFaceDetection();

    uint64_t output_version = 0;
    AnalysisWorker::Output output;
    double last_activity = glfwGetTime();
    double next_capture_retry = 0.0;

    // Waking the display or unlocking the session can leave the stream a
    // zombie: it either stops delivering without an error, or a retry that
    // ran while the screen was locked started a stream bound to the wrong
    // session. Both look alive, so the wake signal forces a restart -
    // cheap on a screen that was just black.
    std::atomic<bool> capture_stale{false};
    ObserveSystemWake([&capture_stale] { capture_stale.store(true); });
    double intensity_flash_until = 0.0;
    double next_preferences_save = -1.0;
    DesktopPoint last_cursor{-1.0, -1.0};
    // The freshest cross-display sample: the async sampler's callback may
    // land on any thread, and may still be in flight at shutdown, so the
    // state it writes is shared ownership.
    struct ScreenSample {
        std::mutex mutex;
        std::optional<FloatColor> color;
    };
    auto screen_sample = std::make_shared<ScreenSample>();
    double next_screen_sample = 0.0;

    const auto is_full_region = [&] {
        return analysis.region.left_percent <= 0.0 && analysis.region.top_percent <= 0.0 &&
               analysis.region.right_percent >= 100.0 && analysis.region.bottom_percent >= 100.0;
    };
    const auto sync_region_border = [&] {
        if (capture_display == 0) return;
        if (is_full_region())
            HideRegionBorder();
        else
            ShowRegionBorder(capture_display, analysis.region);
    };
    // Resets all selection: a pending pick and the drawn region alike.
    // The border sync rides the analysis-dirty path.
    const auto reset_region_to_full = [&] {
        CancelRegionPick();
        analysis.region = RegionOfInterest{};
        analysis_dirty = true;
    };
    // Escape pressed while this application is active but focusless (a
    // border grab activates without focusing); consumed by the frame
    // loop like any other cross-thread signal.
    std::atomic<bool> orphan_escape{false};
    ObserveEscapeWithoutKeyWindow([&orphan_escape] { orphan_escape.store(true); });
    const auto save_preferences = [&] {
        Preferences preferences;
        preferences.vectorscope_gain = analysis.vectorscope.gain;
        preferences.waveform_gain = analysis.waveform.gain;
        preferences.vectorscope_stride = analysis.vectorscope.sampling_stride;
        preferences.waveform_stride = analysis.waveform.sampling_stride;
        preferences.vectorscope_smoothing_ms = vectorscope_smoothing_ms;
        preferences.waveform_smoothing_ms = waveform_smoothing_ms;
        preferences.matrix = analysis.vectorscope.matrix;
        preferences.trace_response = analysis.vectorscope.response;
        preferences.histogram_stride = analysis.histogram.sampling_stride;
        preferences.waveform_mode = analysis.waveform.mode;
        preferences.histogram_per_channel = analysis.histogram.style == HistogramStyle::PerChannel;
        preferences.scope_stack.clear();
        for (const ScopeGlyph kind : scope_stack) preferences.scope_stack += ScopeLetter(kind);
        preferences.show_graticule = show_graticule;
        preferences.vectorscope_zoom = vectorscope_zoom;
        preferences.values_as_percent = values_as_percent;
        preferences.shortcuts = shortcuts;
        glfwGetWindowPos(window, &preferences.window_x, &preferences.window_y);
        glfwGetWindowSize(window, &preferences.window_width, &preferences.window_height);
        SavePreferences(preferences, PreferencesFilePath());
    };
    sync_region_border();

    while (!glfwWindowShouldClose(window)) {
        // Idle: with no new output, no cursor motion, and no interaction,
        // wait for events at a slow tick instead of spinning at refresh.
        if (glfwGetTime() - last_activity > 0.5)
            glfwWaitEventsTimeout(0.1);
        else
            glfwPollEvents();

        // Capture is a service that dies (lock screen, display sleep);
        // restarting it is our job.
        if (g_face_check_requested.exchange(false)) RefreshFacePresence(worker, capture_display);
        if (orphan_escape.exchange(false)) {
            reset_region_to_full();
            last_activity = glfwGetTime();
        }

        if (capture_stale.exchange(false)) {
            std::fprintf(stderr, "sidescopes: restarting capture after wake or unlock\n");
            capture_dead.store(true);
            // Give the session a moment to finish coming back.
            next_capture_retry = glfwGetTime() + 1.0;
        }
        if (permission_granted && capture_dead.load() && glfwGetTime() > next_capture_retry) {
            capture->Stop();
            if (start_capture())
                last_activity = glfwGetTime();
            else
                next_capture_retry = glfwGetTime() + 2.0;
        }

        // With no region drawn, capture follows the display this window
        // sits on: the fallback stays predictable - you always scope the
        // screen you can see the scopes on. A drawn region pins capture
        // to its own display regardless of where the window goes.
        if (permission_granted && !capture_dead.load() && !region_picking && is_full_region()) {
            const auto home_display = display_of_window();
            if (home_display && *home_display != capture_display) {
                desired_display = *home_display;
                capture->Stop();
                if (start_capture()) {
                    last_activity = glfwGetTime();
                } else {
                    capture_dead.store(true);
                    next_capture_retry = glfwGetTime() + 2.0;
                }
            }
        }

        // The window may have moved to a monitor with a different scale.
        const float current_scale = UiScale(window);
        if (current_scale != ui_scale) {
            apply_ui_scale(current_scale);
            last_activity = glfwGetTime();
        }

        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        if (framebuffer_width == 0 || framebuffer_height == 0) continue;
        if (!graphics->BeginFrame(framebuffer_width, framebuffer_height)) continue;

        if (worker.FetchOutput(output_version, output)) {
            if (scope_shown(ScopeGlyph::Vectorscope))
                upload_scope(vectorscope_texture, output.vectorscope_image);
            if (scope_shown(ScopeGlyph::Waveform))
                upload_scope(waveform_texture, output.waveform_image);
            if (scope_shown(ScopeGlyph::WaveformParade))
                upload_scope(waveform_parade_texture, output.waveform_parade_image);
            if (scope_shown(ScopeGlyph::Histogram))
                upload_scope(histogram_texture, output.histogram_image);
            last_activity = glfwGetTime();
        }

        // Publish our own window rectangle (frame pixels, generous chrome
        // margins) so analysis masks it out of change detection.
        const auto frame_size = worker.LatestFrameSize();
        if (frame_size && capture_display != 0) {
            if (const auto geometry = GeometryOfDisplay(capture_display)) {
                int window_x = 0, window_y = 0, window_w = 0, window_h = 0;
                glfwGetWindowPos(window, &window_x, &window_y);
                glfwGetWindowSize(window, &window_w, &window_h);
                const double scale_x = frame_size->width / geometry->width_points;
                const double scale_y = frame_size->height / geometry->height_points;
                // The chrome margins are 100%-scale units like the rest of
                // the interface, so they grow with the monitor's scale.
                const IntRect self_window{
                    static_cast<int>((window_x - geometry->origin_x - 8 * ui_scale) * scale_x),
                    static_cast<int>((window_y - geometry->origin_y - 42 * ui_scale) * scale_y),
                    static_cast<int>((window_w + 16 * ui_scale) * scale_x),
                    static_cast<int>((window_h + 58 * ui_scale) * scale_y)};
                if (self_window.x != analysis.masked_window.x ||
                    self_window.y != analysis.masked_window.y ||
                    self_window.width != analysis.masked_window.width ||
                    self_window.height != analysis.masked_window.height) {
                    analysis.masked_window = self_window;
                    analysis_dirty = true;
                }
            }
        }

        // Cursor color, smoothed per scope with its own rhythm. On the
        // tracked display it reads the capture stream's frame; on every
        // other display a throttled one-shot sample keeps the readout,
        // the markers, and the picker pane alive - even while capture
        // itself is paused.
        std::optional<FloatColor> vectorscope_color;
        std::optional<FloatColor> waveform_color;
        if (capture_display != 0) {
            if (const auto cursor = GlobalCursorPosition()) {
                if (std::abs(cursor->x - last_cursor.x) + std::abs(cursor->y - last_cursor.y) >
                    0.5) {
                    last_cursor = *cursor;
                    last_activity = glfwGetTime();
                }
                std::optional<FloatColor> sampled;
                const bool on_tracked_display =
                    DisplayAtPoint(*cursor).value_or(0) == capture_display;
                if (on_tracked_display && !capture_dead.load() && frame_size) {
                    if (const auto geometry = GeometryOfDisplay(capture_display)) {
                        const int pixel_x =
                            static_cast<int>((cursor->x - geometry->origin_x) * frame_size->width /
                                             geometry->width_points);
                        const int pixel_y =
                            static_cast<int>((cursor->y - geometry->origin_y) * frame_size->height /
                                             geometry->height_points);
                        sampled = worker.SampleFrameColor(pixel_x, pixel_y);
                    }
                } else {
                    if (glfwGetTime() > next_screen_sample) {
                        next_screen_sample = glfwGetTime() + 0.05;
                        SampleScreenColorAsync(*cursor,
                                               [screen_sample](std::optional<FloatColor> color) {
                                                   if (!color) return;
                                                   std::lock_guard lock(screen_sample->mutex);
                                                   screen_sample->color = color;
                                               });
                    }
                    std::lock_guard lock(screen_sample->mutex);
                    sampled = screen_sample->color;
                }
                if (sampled) {
                    vectorscope_marker.SetTimeConstant(vectorscope_smoothing_ms);
                    waveform_marker.SetTimeConstant(waveform_smoothing_ms);
                    vectorscope_color = vectorscope_marker.Update(*sampled, io.DeltaTime);
                    waveform_color = waveform_marker.Update(*sampled, io.DeltaTime);
                }
            }
        }

        // --- adaptive scope detail ---
        // Resolution follows the pane a scope actually gets, and never
        // exceeds what the region can populate: more columns than the
        // region has pixels only spreads samples thin, and a finer
        // chroma grid than the sample count can fill reads as noise.
        {
            int window_w = 0;
            int window_h = 0;
            glfwGetWindowSize(window, &window_w, &window_h);
            const float density =
                window_w > 0 ? static_cast<float>(framebuffer_width) / window_w : 1.0f;
            const auto pane = [&](ScopeGlyph kind) {
                const ImVec2& points = pane_points[static_cast<int>(kind)];
                return ImVec2(points.x * density, points.y * density);
            };
            int region_width = 0;
            if (frame_size) {
                const IntRect region_pixels =
                    analysis.region.ToPixels(frame_size->width, frame_size->height);
                region_width = region_pixels.width;
            }

            int want_columns = analysis.waveform.columns;
            int want_height = analysis.waveform.image_height;
            if (scope_shown(ScopeGlyph::Waveform) || scope_shown(ScopeGlyph::WaveformParade)) {
                const float wf_width =
                    std::max(pane(ScopeGlyph::Waveform).x, pane(ScopeGlyph::WaveformParade).x);
                const float wf_height =
                    std::max(pane(ScopeGlyph::Waveform).y, pane(ScopeGlyph::WaveformParade).y);
                want_columns = wf_width >= 1400.0f ? 2048 : wf_width >= 500.0f ? 1024 : 512;
                if (region_width > 0)
                    want_columns = std::min(want_columns, region_width >= 2048   ? 2048
                                                          : region_width >= 1024 ? 1024
                                                                                 : 512);
                want_height = wf_height >= 560.0f ? 512 : kWaveformLevels;
            }
            int want_vectorscope = analysis.vectorscope.size;
            if (scope_shown(ScopeGlyph::Vectorscope)) {
                // Purely a display resolution: accumulation stays on the
                // 256-code grid and a finer image is interpolated from
                // it, so a sparse region costs nothing extra.
                const ImVec2 scope_pane = pane(ScopeGlyph::Vectorscope);
                const float extent = std::min(scope_pane.x, scope_pane.y);
                want_vectorscope = extent >= 480.0f ? 512 : 256;
            }

            const bool differs = want_columns != analysis.waveform.columns ||
                                 want_height != analysis.waveform.image_height ||
                                 want_vectorscope != analysis.vectorscope.size;
            if (!differs) {
                pending_columns = 0;
            } else if (pending_columns != want_columns || pending_image_height != want_height ||
                       pending_vectorscope != want_vectorscope) {
                pending_columns = want_columns;
                pending_image_height = want_height;
                pending_vectorscope = want_vectorscope;
                detail_pending_since = glfwGetTime();
            } else if (glfwGetTime() - detail_pending_since > 0.4) {
                analysis.waveform.columns = want_columns;
                analysis.waveform.image_height = want_height;
                analysis.vectorscope.size = want_vectorscope;
                analysis_dirty = true;
            }
        }

        // --- frame ---
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        ImGui::Begin("##host", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

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
        const auto image_for = [&](ScopeGlyph kind) -> const ScopeImage& {
            switch (kind) {
                case ScopeGlyph::Vectorscope:
                    return output.vectorscope_image;
                case ScopeGlyph::Waveform:
                    return output.waveform_image;
                case ScopeGlyph::WaveformParade:
                    return output.waveform_parade_image;
                default:
                    return output.histogram_image;
            }
        };
        const auto upload_visible_scopes = [&] {
            if (scope_shown(ScopeGlyph::Vectorscope))
                upload_scope(vectorscope_texture, output.vectorscope_image);
            if (scope_shown(ScopeGlyph::Waveform))
                upload_scope(waveform_texture, output.waveform_image);
            if (scope_shown(ScopeGlyph::WaveformParade))
                upload_scope(waveform_parade_texture, output.waveform_parade_image);
            if (scope_shown(ScopeGlyph::Histogram))
                upload_scope(histogram_texture, output.histogram_image);
        };
        const auto refresh_activated_scope = [&](ScopeGlyph kind) {
            if (kind == ScopeGlyph::ColorPicker) return;
            const uint64_t stale_sequence = image_for(kind).sequence;
            worker.UpdateSettings(analysis);
            const double deadline = glfwGetTime() + 0.08;
            while (glfwGetTime() < deadline) {
                if (worker.FetchOutput(output_version, output) &&
                    image_for(kind).sequence != stale_sequence && image_for(kind).width > 0) {
                    upload_visible_scopes();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            upload_visible_scopes();  // timeout: a stale image beats none
        };
        const auto toggle_scope = [&](ScopeGlyph kind) {
            const auto at = std::find(scope_stack.begin(), scope_stack.end(), kind);
            if (at != scope_stack.end()) {
                if (scope_stack.size() > 1) scope_stack.erase(at);
                sync_enabled_scopes();
            } else {
                scope_stack.push_back(kind);
                sync_enabled_scopes();
                refresh_activated_scope(kind);
            }
            analysis_dirty = true;
        };
        const auto choose_scope = [&](ScopeGlyph kind, bool stack) {
            if (stack) {
                toggle_scope(kind);
            } else {
                const bool was_shown = scope_shown(kind);
                scope_stack.assign(1, kind);
                sync_enabled_scopes();
                if (!was_shown) refresh_activated_scope(kind);
                analysis_dirty = true;
            }
        };
        // The stacking modifier reads the OS's live key state, not the
        // event-tracked one: ImGui's backend re-injects GLFW's cached
        // modifiers on every key press and click, so a Shift key-up
        // swallowed by a system overlay (the screenshot interface) leaves
        // the cache stuck exactly when the user next switches a scope.
        const bool stack_modifier = CurrentModifiers().shift;
        const auto scope_toggle = [&](const char* id, ScopeGlyph kind, const char* tooltip) {
            const char letter[2] = {ScopeLetter(kind), '\0'};
            if (ScopeToggleButton(id, letter, scope_shown(kind), tooltip))
                choose_scope(kind, stack_modifier);
            ImGui::SameLine(0.0f, 2.0f);
        };
        // Tooltips name the configured shortcut, not an assumed one.
        char tooltip[96];
        const auto scope_tooltip = [&](const char* name, const std::string& binding,
                                       const char* extra) {
            std::snprintf(tooltip, sizeof(tooltip), "%s - %s to switch, Shift+%s to stack%s", name,
                          binding.c_str(), binding.c_str(), extra);
            return tooltip;
        };
        scope_toggle("##toggle-vectorscope", ScopeGlyph::Vectorscope,
                     scope_tooltip("Vectorscope", shortcuts.vectorscope, ""));
        scope_toggle(
            "##toggle-waveform", ScopeGlyph::Waveform,
            scope_tooltip("Waveform", shortcuts.waveform, "; styles in the right-click menu"));
        scope_toggle("##toggle-waveform-parade", ScopeGlyph::WaveformParade,
                     scope_tooltip("RGB parade", shortcuts.parade, ""));
        scope_toggle("##toggle-histogram", ScopeGlyph::Histogram,
                     scope_tooltip("Histogram", shortcuts.histogram, ""));
        scope_toggle("##toggle-color-picker", ScopeGlyph::ColorPicker,
                     scope_tooltip("Color picker", shortcuts.color_picker, ""));
        ImGui::SameLine(0.0f, 8.0f);

        // Keyboard shortcuts mirror the toolbar and region tools.
        std::optional<RegionPickerMode> want_region_pick;
        const auto pressed = [&](const std::string& binding) {
            const ImGuiKey key = key_for(binding);
            return key != ImGuiKey_None && ImGui::IsKeyPressed(key, false);
        };
        if (!io.WantTextInput) {
            if (pressed(shortcuts.vectorscope))
                choose_scope(ScopeGlyph::Vectorscope, stack_modifier);
            if (pressed(shortcuts.waveform)) choose_scope(ScopeGlyph::Waveform, stack_modifier);
            if (pressed(shortcuts.parade)) choose_scope(ScopeGlyph::WaveformParade, stack_modifier);
            if (pressed(shortcuts.histogram)) choose_scope(ScopeGlyph::Histogram, stack_modifier);
            if (pressed(shortcuts.color_picker))
                choose_scope(ScopeGlyph::ColorPicker, stack_modifier);
            if (pressed(shortcuts.pick_window)) want_region_pick = RegionPickerMode::PickWindows;
            if (pressed(shortcuts.draw_region)) want_region_pick = RegionPickerMode::Draw;
            if (SupportsFaceDetection() && pressed(shortcuts.pick_faces))
                want_region_pick = RegionPickerMode::PickFaces;
            if (pressed(shortcuts.pin_color)) {
                if (stack_modifier) {
                    if (output.region_average_valid) pin_reference_color(output.region_average);
                } else {
                    pin_reference_color(vectorscope_color);
                }
            }
            if (pressed(shortcuts.vectorscope_zoom))
                vectorscope_zoom = vectorscope_zoom >= 4 ? 1 : vectorscope_zoom * 2;
            if (pressed(shortcuts.full_region)) reset_region_to_full();
        }

        std::snprintf(tooltip, sizeof(tooltip), "Pick a window (%s)",
                      shortcuts.pick_window.c_str());
        if (IconButton("##pick-region", RegionIcon::PickHand, tooltip))
            want_region_pick = RegionPickerMode::PickWindows;
        ImGui::SameLine(0.0f, 2.0f);
        std::snprintf(tooltip, sizeof(tooltip), "Draw an area (%s)", shortcuts.draw_region.c_str());
        if (IconButton("##draw-region", RegionIcon::Crosshair, tooltip))
            want_region_pick = RegionPickerMode::Draw;
        ImGui::SameLine(0.0f, 2.0f);
        if (SupportsFaceDetection()) {
            const bool none_found = g_faces_on_screen.load() == 0;
            std::snprintf(tooltip, sizeof(tooltip), "Pick a face (%s)%s",
                          shortcuts.pick_faces.c_str(),
                          none_found ? " - none on screen right now" : "");
            if (IconButton("##pick-face", RegionIcon::Face, tooltip, none_found))
                want_region_pick = RegionPickerMode::PickFaces;
            ImGui::SameLine(0.0f, 2.0f);
        }
        if (!is_full_region()) {
            if (IconButton("##full-region", RegionIcon::Expand, "Reset to full screen (Esc)"))
                reset_region_to_full();
            ImGui::SameLine(0.0f, 2.0f);
        }

        // The readout yields before the icons do: on a narrow window it
        // would right-align on top of the toolbar buttons, and whoever
        // wants the window that small still needs the buttons - the
        // cursor color has the vectorscope marker and the context menu.
        const float toolbar_end = ImGui::GetCursorPosX();
        if (vectorscope_color) {
            const FloatColor& color = *vectorscope_color;
            // The readout's geometry never follows its digits: each value
            // gets a column sized for the widest it can be and is
            // right-aligned inside it, so neither the swatch nor the
            // numbers wander as the cursor moves across the screen.
            const float column_width = ImGui::CalcTextSize(values_as_percent ? "100%" : "255").x;
            const float column_gap = ImGui::CalcTextSize(" ").x;
            const float swatch = ImGui::GetTextLineHeight();
            const float text_width = 3 * column_width + 2 * column_gap;
            const float readout_start =
                ImGui::GetWindowContentRegionMax().x - (text_width + swatch + 6);
            if (readout_start >= toolbar_end + 8) {
                ImGui::SameLine(readout_start);
                ImGui::ColorButton(
                    "##cursor-color",
                    ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f),
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(swatch, swatch));
                const float columns_start = readout_start + swatch + 6;
                const float channels[3] = {color.r, color.g, color.b};
                for (int channel = 0; channel < 3; ++channel) {
                    char value[8];
                    if (values_as_percent)
                        std::snprintf(value, sizeof(value), "%.0f%%", channels[channel] / 2.55);
                    else
                        std::snprintf(value, sizeof(value), "%.0f", channels[channel]);
                    const float column_start =
                        columns_start + channel * (column_width + column_gap);
                    ImGui::SameLine(column_start + column_width - ImGui::CalcTextSize(value).x);
                    ImGui::TextUnformatted(value);
                }
            } else {
                ImGui::NewLine();
            }
        } else {
            ImGui::NewLine();
        }

        // Scroll over a scope adjusts its intensity; double-click resets.
        static float* flash_intensity = nullptr;
        const auto scope_gestures = [&](const DrawnScope& scope, float& intensity, float& gain,
                                        float default_gain, float intensity_shift = 0.0f) {
            if (!ImGui::IsItemHovered()) return;
            if (io.MouseWheel != 0.0f) {
                intensity = std::clamp(intensity + 2.0f * io.MouseWheel, 0.0f, 100.0f);
                gain = TraceGainFromIntensity(intensity, intensity_shift);
                analysis_dirty = true;
                intensity_flash_until = glfwGetTime() + 1.2;
                flash_intensity = &intensity;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                gain = default_gain;
                intensity = IntensityFromTraceGain(gain, intensity_shift);
                analysis_dirty = true;
                intensity_flash_until = glfwGetTime() + 1.2;
                flash_intensity = &intensity;
            }
            if (glfwGetTime() < intensity_flash_until && flash_intensity == &intensity) {
                char text[32];
                std::snprintf(text, sizeof(text), "intensity %.0f%%", intensity);
                ImGui::GetWindowDrawList()->AddText(ImVec2(scope.origin.x + 8, scope.origin.y + 6),
                                                    IM_COL32(235, 235, 240, 220), text);
            }
        };

        const auto draw_vectorscope = [&] {
            const DrawnScope scope =
                DrawScopeImage(*vectorscope_texture, true, static_cast<float>(vectorscope_zoom));
            scope_gestures(scope, vectorscope_intensity, analysis.vectorscope.gain, 3.0f,
                           kVectorscopeIntensityShift);
            // Zoomed overlays run past the pane; clip them to it.
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(scope.origin,
                               ImVec2(scope.origin.x + scope.size.x, scope.origin.y + scope.size.y),
                               true);
            if (show_graticule)
                DrawVectorscopeOverlay(scope, BuildVectorscopeGraticule(projection_vectorscope));
            for (const FloatColor& pinned : pinned_colors) {
                if (const auto point = projection_vectorscope.Project(pinned))
                    DrawPointMarker(scope, *point, IM_COL32(230, 170, 90, 230));
            }
            if (vectorscope_color) {
                if (const auto point = projection_vectorscope.Project(*vectorscope_color))
                    DrawPointMarker(scope, *point, IM_COL32(255, 255, 255, 255));
            }
            draw->PopClipRect();
            if (vectorscope_zoom > 1) {
                char badge[4] = {static_cast<char>('0' + vectorscope_zoom), 'x', '\0'};
                draw->AddText(ImVec2(scope.origin.x + scope.size.x - 26, scope.origin.y + 6),
                              kGraticuleLabel, badge);
            }
        };
        // The waveform flavors share gain and graticule; the cursor
        // markers differ - channel levels on RGB and parade, one luma
        // level on luma.
        const auto draw_channel_markers = [&](const DrawnScope& scope) {
            if (!waveform_color) return;
            ChannelMarker markers[3];
            const int count = GroupChannelMarkers(*waveform_color, markers);
            for (int i = 0; i < count; ++i)
                DrawLevelMarker(scope, (255.0f - markers[i].value) / 255.0f, markers[i].color);
        };
        // The parade separates the channels into thirds, so each marker
        // stays a single color inside its own channel's column.
        const auto draw_parade_markers = [&](const DrawnScope& scope) {
            if (!waveform_color) return;
            const float channels[3] = {waveform_color->r, waveform_color->g, waveform_color->b};
            for (int channel = 0; channel < 3; ++channel)
                DrawLevelMarker(scope, (255.0f - channels[channel]) / 255.0f,
                                ChannelMaskColor(1 << channel), channel / 3.0f,
                                (channel + 1) / 3.0f);
        };
        const auto draw_waveform = [&](ScopeGlyph kind) {
            const ScopeTexture& texture =
                kind == ScopeGlyph::Waveform ? *waveform_texture : *waveform_parade_texture;
            const DrawnScope scope = DrawScopeImage(texture, false);
            scope_gestures(scope, waveform_intensity, analysis.waveform.gain, 0.05f);
            if (show_graticule) DrawWaveformOverlay(scope);
            if (kind == ScopeGlyph::Waveform &&
                (analysis.waveform.mode == WaveformMode::Luma ||
                 analysis.waveform.mode == WaveformMode::ColoredLuma)) {
                if (waveform_color) {
                    if (const auto point = projection_waveform.Project(*waveform_color))
                        DrawLevelMarker(scope, point->y, IM_COL32(255, 220, 80, 220));
                }
            } else if (kind == ScopeGlyph::Waveform) {
                draw_channel_markers(scope);
            } else {
                draw_parade_markers(scope);
            }
        };

        const auto draw_histogram = [&] {
            // No intensity gesture here: the histogram's scale adjusts
            // itself, the way every editor draws it.
            const DrawnScope scope = DrawScopeImage(*histogram_texture, false);
            if (show_graticule) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                for (int quarter = 0; quarter <= 4; ++quarter) {
                    const float x = scope.origin.x + scope.size.x * quarter / 4.0f;
                    draw->AddLine(ImVec2(x, scope.origin.y),
                                  ImVec2(x, scope.origin.y + scope.size.y),
                                  quarter % 2 == 0 ? kGraticuleMajor : kGraticuleMinor);
                }
            }
            if (vectorscope_color) {
                if (analysis.histogram.style == HistogramStyle::PerChannel) {
                    // Each channel's marker stays a single color inside
                    // its own band.
                    const float channels[3] = {vectorscope_color->r, vectorscope_color->g,
                                               vectorscope_color->b};
                    for (int channel = 0; channel < 3; ++channel)
                        DrawValueMarker(scope, channels[channel] / 255.0f,
                                        ChannelMaskColor(1 << channel), channel / 3.0f,
                                        (channel + 1) / 3.0f);
                } else {
                    ChannelMarker markers[3];
                    const int count = GroupChannelMarkers(*vectorscope_color, markers);
                    for (int i = 0; i < count; ++i)
                        DrawValueMarker(scope, markers[i].value / 255.0f, markers[i].color);
                }
            }
        };

        // When nothing can be captured, the scope area explains why and how
        // to fix it instead of drawing empty instruments; a non-technical
        // user should never face a blank vectorscope.
        const auto draw_capture_help = [&](const char* headline,
                                           const std::vector<std::string>& lines,
                                           bool offer_settings) {
            const ImVec2 area = ImGui::GetContentRegionAvail();
            const float line_height = ImGui::GetTextLineHeightWithSpacing();
            const float block_height = line_height * (2.0f + static_cast<float>(lines.size())) +
                                       (offer_settings ? line_height * 2.0f : 0.0f);
            ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (area.y - block_height) / 2.0f)));
            const auto centered_text = [&](const char* text) {
                const float width = ImGui::CalcTextSize(text).x;
                ImGui::SetCursorPosX(
                    std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
                ImGui::TextUnformatted(text);
            };
            centered_text(headline);
            ImGui::Dummy(ImVec2(0.0f, line_height * 0.4f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            for (const std::string& line : lines) centered_text(line.c_str());
            ImGui::PopStyleColor();
            if (offer_settings) {
                ImGui::Dummy(ImVec2(0.0f, line_height * 0.6f));
                const char* label = "Open System Settings";
                const float width =
                    ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SetCursorPosX(
                    std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
                if (ImGui::Button(label)) OpenScreenRecordingSettings();
            }
        };

        // The color picker pane: the sampled cursor color as a large
        // swatch with its values spelled out three ways at once - 0-255,
        // percent, and hex - because matching a reference means never
        // converting in your head. Clicking the swatch or the hex line
        // copies the hex; the session's pinned colors (P) ride along as
        // small swatches with the same click.
        const auto draw_color_picker = [&] {
            const ImVec2 area = ImGui::GetContentRegionAvail();
            const float line_height = ImGui::GetTextLineHeightWithSpacing();
            if (!vectorscope_color) {
                ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (area.y - line_height) / 2.0f)));
                const char* hint = "no color under the cursor yet";
                const float width = ImGui::CalcTextSize(hint).x;
                ImGui::SetCursorPosX(
                    std::max(0.0f, (ImGui::GetWindowContentRegionMax().x - width) / 2.0f));
                ImGui::TextDisabled("%s", hint);
                return;
            }
            const FloatColor& color = *vectorscope_color;
            const int red = static_cast<int>(std::lround(std::clamp(color.r, 0.0f, 255.0f)));
            const int green = static_cast<int>(std::lround(std::clamp(color.g, 0.0f, 255.0f)));
            const int blue = static_cast<int>(std::lround(std::clamp(color.b, 0.0f, 255.0f)));
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", red, green, blue);
            const float values_height = line_height * (pinned_colors.empty() ? 4.0f : 5.0f) +
                                        ImGui::GetStyle().ItemSpacing.y;
            const float swatch_height =
                std::max(ImGui::GetTextLineHeight() * 2.0f, area.y - values_height);
            const ImVec4 swatch(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f);
            if (ImGui::ColorButton("##picker-swatch", swatch,
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                                   ImVec2(area.x, swatch_height)))
                ImGui::SetClipboardText(hex);
            ImGui::SetItemTooltip("%s - click to copy", hex);
            ImGui::Text("R %3d  %5.1f%%", red, color.r / 2.55f);
            ImGui::Text("G %3d  %5.1f%%", green, color.g / 2.55f);
            ImGui::Text("B %3d  %5.1f%%", blue, color.b / 2.55f);
            ImGui::TextUnformatted(hex);
            if (ImGui::IsItemClicked()) ImGui::SetClipboardText(hex);
            ImGui::SetItemTooltip("click to copy");
            if (!pinned_colors.empty()) {
                const float small = ImGui::GetTextLineHeight();
                for (std::size_t index = 0; index < pinned_colors.size(); ++index) {
                    const FloatColor& pinned = pinned_colors[index];
                    char pin_id[24];
                    std::snprintf(pin_id, sizeof(pin_id), "##pinned-%d", static_cast<int>(index));
                    char pin_hex[8];
                    std::snprintf(pin_hex, sizeof(pin_hex), "#%02X%02X%02X",
                                  static_cast<int>(std::lround(pinned.r)),
                                  static_cast<int>(std::lround(pinned.g)),
                                  static_cast<int>(std::lround(pinned.b)));
                    if (index > 0) ImGui::SameLine(0.0f, 4.0f);
                    if (ImGui::ColorButton(
                            pin_id,
                            ImVec4(pinned.r / 255.0f, pinned.g / 255.0f, pinned.b / 255.0f, 1.0f),
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                            ImVec2(small, small)))
                        ImGui::SetClipboardText(pin_hex);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        pinned_menu_index = static_cast<int>(index);
                        ImGui::OpenPopup("##pinned-menu");
                    }
                    ImGui::SetItemTooltip("%s - click to copy, right-click to manage", pin_hex);
                }
                // Managing a pin happens where the pin lives; the
                // app-wide native menu yields to this popup.
                if (ImGui::BeginPopup("##pinned-menu")) {
                    if (pinned_menu_index >= 0 &&
                        pinned_menu_index < static_cast<int>(pinned_colors.size())) {
                        const std::size_t chosen = static_cast<std::size_t>(pinned_menu_index);
                        char chosen_hex[8];
                        std::snprintf(chosen_hex, sizeof(chosen_hex), "#%02X%02X%02X",
                                      static_cast<int>(std::lround(pinned_colors[chosen].r)),
                                      static_cast<int>(std::lround(pinned_colors[chosen].g)),
                                      static_cast<int>(std::lround(pinned_colors[chosen].b)));
                        if (ImGui::MenuItem(chosen_hex)) ImGui::SetClipboardText(chosen_hex);
                        if (ImGui::MenuItem("Remove"))
                            pinned_colors.erase(pinned_colors.begin() + static_cast<long>(chosen));
                        ImGui::Separator();
                        if (ImGui::MenuItem("Clear All")) pinned_colors.clear();
                    }
                    ImGui::EndPopup();
                }
            }
        };

        // The enabled scopes stack in a fixed order, splitting the window
        // along its longer axis.
        // Which pane is under the cursor decides which options the
        // context menu shows; rects refresh as the panes draw.
        ImVec4 pane_rects[5] = {};
        const auto draw_scope = [&](ScopeGlyph kind) {
            pane_points[static_cast<int>(kind)] = ImGui::GetContentRegionAvail();
            const ImVec2 pane_min = ImGui::GetCursorScreenPos();
            const ImVec2 pane_avail = ImGui::GetContentRegionAvail();
            pane_rects[static_cast<int>(kind)] = ImVec4(
                pane_min.x, pane_min.y, pane_min.x + pane_avail.x, pane_min.y + pane_avail.y);
            if (kind == ScopeGlyph::Vectorscope)
                draw_vectorscope();
            else if (kind == ScopeGlyph::Histogram)
                draw_histogram();
            else if (kind == ScopeGlyph::ColorPicker)
                draw_color_picker();
            else
                draw_waveform(kind);
        };
        const int pane_count = static_cast<int>(scope_stack.size());
        const ImVec2 area = ImGui::GetContentRegionAvail();
        if (!permission_granted) {
            draw_capture_help("SideScopes cannot see the screen",
                              {
                                  "macOS requires the Screen Recording permission.",
                                  "",
                                  "1. Click the button below",
                                  "2. Turn on SideScopes in the list",
                                  "3. Quit and reopen SideScopes",
                              },
                              true);
        } else if (capture_dead.load()) {
            std::string status;
            {
                std::lock_guard lock(status_mutex);
                status = capture_status;
            }
            draw_capture_help("Screen capture was interrupted",
                              {status, "Reconnecting automatically..."}, false);
        } else if (pane_count <= 1) {
            if (pane_count == 1) draw_scope(scope_stack.front());
        } else {
            const bool horizontal = area.x >= area.y;
            const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
            const ImVec2 pane_size =
                horizontal ? ImVec2((area.x - spacing.x * (pane_count - 1)) / pane_count, area.y)
                           : ImVec2(area.x, (area.y - spacing.y * (pane_count - 1)) / pane_count);
            const char* pane_ids[5] = {"##pane0", "##pane1", "##pane2", "##pane3", "##pane4"};
            for (int pane = 0; pane < pane_count; ++pane) {
                ImGui::BeginChild(pane_ids[pane], pane_size);
                draw_scope(scope_stack[static_cast<std::size_t>(pane)]);
                ImGui::EndChild();
                if (horizontal && pane + 1 < pane_count) ImGui::SameLine();
            }
        }

        // Right-click: the native menu carries the modes and toggles.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            using Kind = NativeMenuItem::Kind;
            // The menu shows every scope but only the options of the pane
            // under the cursor: right-clicking the waveform offers its
            // styles, the vectorscope its matrix, response, and zoom.
            // Clicking the toolbar or background offers everything.
            const ImVec2 mouse = ImGui::GetMousePos();
            int clicked_pane = -1;
            for (int pane = 0; pane < 5; ++pane) {
                const ImVec4& rect = pane_rects[pane];
                if (rect.z <= rect.x || rect.w <= rect.y) continue;
                if (mouse.x >= rect.x && mouse.x < rect.z && mouse.y >= rect.y && mouse.y < rect.w)
                    clicked_pane = pane;
            }
            const auto over = [&](ScopeGlyph kind) {
                return clicked_pane < 0 || clicked_pane == static_cast<int>(kind);
            };

            std::vector<NativeMenuItem> menu;
            const auto action = [&](const char* label, int id, bool checked,
                                    std::string shortcut = "") {
                menu.push_back({Kind::Action, label, id, checked, std::move(shortcut)});
            };
            const auto separator = [&] { menu.push_back({Kind::Separator, "", -1, false, ""}); };
            const auto submenu = [&](const char* label) {
                menu.push_back({Kind::SubmenuBegin, label, -1, false, ""});
            };
            const auto end_submenu = [&] { menu.push_back({Kind::SubmenuEnd, "", -1, false, ""}); };

            action("Vectorscope", kMenuShowVectorscope, scope_shown(ScopeGlyph::Vectorscope),
                   shortcut_label(shortcuts.vectorscope));
            action("Waveform", kMenuShowWaveform, scope_shown(ScopeGlyph::Waveform),
                   shortcut_label(shortcuts.waveform));
            action("RGB Parade", kMenuShowWaveformParade, scope_shown(ScopeGlyph::WaveformParade),
                   shortcut_label(shortcuts.parade));
            action("Histogram", kMenuShowHistogram, scope_shown(ScopeGlyph::Histogram),
                   shortcut_label(shortcuts.histogram));
            action("Color Picker", kMenuShowColorPicker, scope_shown(ScopeGlyph::ColorPicker),
                   shortcut_label(shortcuts.color_picker));

            bool options_shown = false;
            const auto options_separator = [&] {
                if (!options_shown) separator();
                options_shown = true;
            };
            if (over(ScopeGlyph::Waveform)) {
                options_separator();
                submenu("Waveform Style");
                action("RGB", kMenuWaveformStyleRgb, analysis.waveform.mode == WaveformMode::Rgb);
                action("Luma", kMenuWaveformStyleLuma,
                       analysis.waveform.mode == WaveformMode::Luma);
                action("Luma (Colored)", kMenuWaveformStyleColoredLuma,
                       analysis.waveform.mode == WaveformMode::ColoredLuma);
                end_submenu();
            }
            if (over(ScopeGlyph::Vectorscope)) {
                options_separator();
                const bool bt601 = analysis.vectorscope.matrix == ChromaMatrix::Bt601;
                submenu("Vectorscope Matrix");
                action("BT.601", kMenuMatrixBt601, bt601);
                action("BT.709", kMenuMatrixBt709, !bt601);
                end_submenu();
                submenu("Trace Response");
                action("Boosted", kMenuTraceBoosted,
                       analysis.vectorscope.response == TraceResponse::Boosted);
                action("Linear", kMenuTraceLinear,
                       analysis.vectorscope.response == TraceResponse::Linear);
                end_submenu();
                submenu("Zoom");
                action("1x", kMenuZoom1, vectorscope_zoom == 1,
                       shortcut_label(shortcuts.vectorscope_zoom));
                action("2x", kMenuZoom2, vectorscope_zoom == 2,
                       shortcut_label(shortcuts.vectorscope_zoom));
                action("4x", kMenuZoom4, vectorscope_zoom == 4,
                       shortcut_label(shortcuts.vectorscope_zoom));
                end_submenu();
            }
            if (over(ScopeGlyph::Histogram)) {
                options_separator();
                submenu("Histogram Style");
                action("Combined", kMenuHistogramCombined,
                       analysis.histogram.style == HistogramStyle::Combined);
                action("Per Channel", kMenuHistogramPerChannel,
                       analysis.histogram.style == HistogramStyle::PerChannel);
                end_submenu();
            }

            separator();
            action("Pick Window or Photo...", kMenuSelectRegion, false,
                   shortcut_label(shortcuts.pick_window));
            action("Draw Area...", kMenuDrawRegion, false, shortcut_label(shortcuts.draw_region));
            if (SupportsFaceDetection())
                action("Find Faces...", kMenuPickFaces, false,
                       shortcut_label(shortcuts.pick_faces));
            action("Watch Full Screen", kMenuFullScreenRegion, is_full_region(),
                   shortcut_label(shortcuts.full_region));

            separator();
            action("Graticule", kMenuToggleGraticule, show_graticule);

            // Pins mark the vectorscope and the color picker; on the other
            // panes the actions would only puzzle.
            const bool pins_apply = over(ScopeGlyph::Vectorscope) || over(ScopeGlyph::ColorPicker);
            if (pins_apply) {
                if (vectorscope_color || output.region_average_valid || !pinned_colors.empty())
                    separator();
                if (vectorscope_color)
                    action("Pin Cursor Color", kMenuPinCursorColor, false,
                           shortcut_label(shortcuts.pin_color));
                if (output.region_average_valid)
                    action("Pin Region Average", kMenuPinRegionAverage, false,
                           "Shift+" + shortcut_label(shortcuts.pin_color));
                if (!pinned_colors.empty())
                    action("Clear Pinned Markers", kMenuClearPinnedMarkers, false);
            }

            separator();
            action("Settings...", kMenuOpenSettings, false);
            action("Quit", kMenuQuit, false);

            switch (ShowNativeContextMenu(menu)) {
                case kMenuShowVectorscope:
                    toggle_scope(ScopeGlyph::Vectorscope);
                    break;
                case kMenuShowWaveform:
                    toggle_scope(ScopeGlyph::Waveform);
                    break;
                case kMenuShowWaveformParade:
                    toggle_scope(ScopeGlyph::WaveformParade);
                    break;
                case kMenuWaveformStyleRgb:
                    analysis.waveform.mode = WaveformMode::Rgb;
                    analysis_dirty = true;
                    break;
                case kMenuWaveformStyleLuma:
                    analysis.waveform.mode = WaveformMode::Luma;
                    analysis_dirty = true;
                    break;
                case kMenuWaveformStyleColoredLuma:
                    analysis.waveform.mode = WaveformMode::ColoredLuma;
                    analysis_dirty = true;
                    break;
                case kMenuHistogramCombined:
                    analysis.histogram.style = HistogramStyle::Combined;
                    analysis_dirty = true;
                    break;
                case kMenuHistogramPerChannel:
                    analysis.histogram.style = HistogramStyle::PerChannel;
                    analysis_dirty = true;
                    break;
                case kMenuShowHistogram:
                    toggle_scope(ScopeGlyph::Histogram);
                    break;
                case kMenuShowColorPicker:
                    toggle_scope(ScopeGlyph::ColorPicker);
                    break;
                case kMenuMatrixBt601:
                    analysis.vectorscope.matrix = ChromaMatrix::Bt601;
                    analysis_dirty = true;
                    break;
                case kMenuMatrixBt709:
                    analysis.vectorscope.matrix = ChromaMatrix::Bt709;
                    analysis_dirty = true;
                    break;
                case kMenuTraceBoosted:
                    analysis.vectorscope.response = TraceResponse::Boosted;
                    analysis_dirty = true;
                    break;
                case kMenuTraceLinear:
                    analysis.vectorscope.response = TraceResponse::Linear;
                    analysis_dirty = true;
                    break;
                case kMenuSelectRegion:
                    want_region_pick = RegionPickerMode::PickWindows;
                    break;
                case kMenuDrawRegion:
                    want_region_pick = RegionPickerMode::Draw;
                    break;
                case kMenuPickFaces:
                    want_region_pick = RegionPickerMode::PickFaces;
                    break;
                case kMenuZoom1:
                    vectorscope_zoom = 1;
                    break;
                case kMenuZoom2:
                    vectorscope_zoom = 2;
                    break;
                case kMenuZoom4:
                    vectorscope_zoom = 4;
                    break;
                case kMenuFullScreenRegion:
                    reset_region_to_full();
                    break;
                case kMenuToggleGraticule:
                    show_graticule = !show_graticule;
                    break;
                case kMenuPinCursorColor:
                    pin_reference_color(vectorscope_color);
                    break;
                case kMenuPinRegionAverage:
                    if (output.region_average_valid) pin_reference_color(output.region_average);
                    break;
                case kMenuClearPinnedMarkers:
                    pinned_colors.clear();
                    break;
                case kMenuOpenSettings:
                    show_settings = true;
                    break;
                case kMenuQuit:
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                    break;
                default:
                    break;
            }
            last_activity = glfwGetTime();
            next_preferences_save = glfwGetTime() + 1.0;
        }

        ImGui::End();
        ImGui::PopStyleVar();

        if (show_settings) {
            ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
            ImGui::Begin("Settings", &show_settings, ImGuiWindowFlags_NoCollapse);
            {
                std::lock_guard lock(status_mutex);
                ImGui::TextWrapped("capture: %s", capture_status.c_str());
            }
            ImGui::Text("analysis %.2f ms | frames %llu | ui %.0f fps",
                        output.accumulate_milliseconds,
                        static_cast<unsigned long long>(output.frames_processed),
                        static_cast<double>(io.Framerate));
            ImGui::Separator();
            ImGui::TextDisabled("vectorscope");
            if (ImGui::SliderFloat("intensity##v", &vectorscope_intensity, 0.0f, 100.0f,
                                   "%.0f%%")) {
                analysis.vectorscope.gain =
                    TraceGainFromIntensity(vectorscope_intensity, kVectorscopeIntensityShift);
                analysis_dirty = true;
            }
            analysis_dirty |=
                ImGui::SliderInt("sampling 1:N##v", &analysis.vectorscope.sampling_stride, 1, 8);
            ImGui::SliderFloat("smoothing ms##v", &vectorscope_smoothing_ms, 0.0f, 500.0f, "%.0f");
            ImGui::TextDisabled("waveform");
            if (ImGui::SliderFloat("intensity##w", &waveform_intensity, 0.0f, 100.0f, "%.0f%%")) {
                analysis.waveform.gain = TraceGainFromIntensity(waveform_intensity);
                analysis_dirty = true;
            }
            analysis_dirty |=
                ImGui::SliderInt("sampling 1:N##w", &analysis.waveform.sampling_stride, 1, 8);
            ImGui::SliderFloat("smoothing ms##w", &waveform_smoothing_ms, 0.0f, 500.0f, "%.0f");
            ImGui::TextDisabled("modes and toggles: right-click a scope");
            ImGui::End();
        }

        if (ImGui::IsAnyItemActive()) {
            last_activity = glfwGetTime();
            next_preferences_save = glfwGetTime() + 1.0;
        }

        ImGui::Render();
        graphics->EndFrame();

        // The blocking overlay runs after the frame is submitted; capture and
        // analysis keep flowing underneath.
        if (region_picking && want_region_pick) {
            // The toolbar keeps working mid-pick: choosing a selection tool
            // switches the active picker's mode instead of stacking one.
            SetRegionPickMode(*want_region_pick);
            want_region_pick.reset();
        }
        if (want_region_pick && capture_display != 0) {
            HideRegionBorder();
            // The previous region's border must not leak into the analyzed
            // frame: its strokes read as rectangle edges and cut suggestions
            // short at the old region. The latest captured frame may predate
            // the hide, so wait briefly for one taken after the border left
            // the screen. The 60 ms floor outlasts an in-flight pre-hide
            // frame's capture-to-delivery; the 300 ms cap keeps the picker
            // responsive if the capture stream has stalled.
            if (!is_full_region()) {
                uint64_t stale_sequence = 0;
                worker.WithLatestFrame(
                    [&](const FrameView& view) { stale_sequence = view.sequence; });
                const double hidden_at = glfwGetTime();
                for (;;) {
                    const double elapsed = glfwGetTime() - hidden_at;
                    if (elapsed >= 0.3) break;
                    uint64_t sequence = stale_sequence;
                    worker.WithLatestFrame(
                        [&](const FrameView& view) { sequence = view.sequence; });
                    // Inequality, not greater-than: a freshly switched
                    // stream counts its frames from one again.
                    if (sequence != stale_sequence && elapsed >= 0.06) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                }
            }
            // The offer, per display: the visible application windows,
            // frontmost first - window rectangles come from the operating
            // system and are exact - plus, behind their own key, the faces
            // the platform detector finds in the current frame. Faces are
            // offered on the tracked display only: that is the display
            // whose pixels we hold.
            const auto window_suggestions_for =
                [&](uint32_t display_id) -> std::vector<SuggestedRegion> {
                std::vector<WindowRegion> window_regions;
                const auto geometry = GeometryOfDisplay(display_id);
                if (geometry) {
                    // Frontmost windows only, skipping ones mostly hidden behind
                    // fronter windows: the user is not scoping what they cannot
                    // see, and invisible system windows have no business here.
                    const std::vector<DesktopWindow> on_screen = OnScreenWindows(display_id);
                    const auto contained_fraction = [](const DesktopWindow& inner,
                                                       const DesktopWindow& outer) {
                        const double left = std::max(inner.x, outer.x);
                        const double top = std::max(inner.y, outer.y);
                        const double right = std::min(inner.x + inner.width, outer.x + outer.width);
                        const double bottom =
                            std::min(inner.y + inner.height, outer.y + outer.height);
                        if (right <= left || bottom <= top) return 0.0;
                        return (right - left) * (bottom - top) / (inner.width * inner.height);
                    };
                    std::vector<DesktopWindow> visible_windows;
                    std::vector<DesktopWindow> auxiliary_windows;
                    for (const DesktopWindow& candidate : on_screen) {
                        constexpr int kMaxWindowSuggestions = 5;
                        if (static_cast<int>(visible_windows.size()) >= kMaxWindowSuggestions)
                            break;
                        // A window living mostly inside a bigger window of the
                        // same application is an auxiliary surface - Lightroom
                        // draws its panels and its loupe info overlay as
                        // borderless windows over the main one - and picking it
                        // is never meant. It is remembered as chrome: the
                        // detector masks it out, so panels and overlays neither
                        // spawn candidates nor interrupt the photograph's
                        // borders.
                        bool auxiliary = false;
                        for (const DesktopWindow& other : on_screen) {
                            if (&other == &candidate || other.application != candidate.application)
                                continue;
                            if (other.width * other.height <= candidate.width * candidate.height)
                                continue;
                            if (contained_fraction(candidate, other) > 0.9) {
                                auxiliary = true;
                                break;
                            }
                        }
                        if (auxiliary) {
                            auxiliary_windows.push_back(candidate);
                            continue;
                        }
                        bool mostly_covered = false;
                        for (const DesktopWindow& front : visible_windows) {
                            if (contained_fraction(candidate, front) > 0.8) {
                                mostly_covered = true;
                                break;
                            }
                        }
                        if (!mostly_covered) visible_windows.push_back(candidate);
                    }

                    for (const DesktopWindow& visible : visible_windows) {
                        WindowRegion region;
                        region.region.left_percent = std::clamp(
                            (visible.x - geometry->origin_x) / geometry->width_points * 100.0, 0.0,
                            100.0);
                        region.region.top_percent = std::clamp(
                            (visible.y - geometry->origin_y) / geometry->height_points * 100.0, 0.0,
                            100.0);
                        region.region.right_percent =
                            std::clamp((visible.x + visible.width - geometry->origin_x) /
                                           geometry->width_points * 100.0,
                                       0.0, 100.0);
                        region.region.bottom_percent =
                            std::clamp((visible.y + visible.height - geometry->origin_y) /
                                           geometry->height_points * 100.0,
                                       0.0, 100.0);
                        region.application = visible.application;
                        window_regions.push_back(std::move(region));
                    }
                }
                return BuildRegionSuggestions(window_regions);
            };

            std::vector<SuggestedRegion> face_suggestions;
            if (SupportsFaceDetection()) {
                worker.WithLatestFrame([&](const FrameView& view) {
                    const auto geometry = GeometryOfDisplay(capture_display);
                    const float pixels_per_point =
                        geometry ? static_cast<float>(view.width / geometry->width_points) : 1.0f;
                    face_suggestions = BuildFaceSuggestions(DetectFaces(view, pixels_per_point),
                                                            view.width, view.height);
                    g_faces_on_screen.store(static_cast<int>(face_suggestions.size()));
                });
            }

            std::vector<PickerDisplay> picker_displays;
            for (const CaptureTarget& target : capture->ListTargets()) {
                PickerDisplay entry;
                entry.display_id = target.display_id;
                entry.windows = window_suggestions_for(target.display_id);
                if (target.display_id == capture_display) entry.faces = face_suggestions;
                picker_displays.push_back(std::move(entry));
            }

            // Field diagnosis: dump exactly what the pipeline saw. Enable
            // with `launchctl setenv SIDESCOPES_DEBUG_SUGGESTIONS 1`.
            if (DebugSuggestionsRequested()) {
                std::FILE* report = OpenDebugFile("/tmp/sidescopes-suggestions.txt", "w");
                if (report) {
                    for (const auto& entry : picker_displays)
                        for (const auto& suggestion : entry.windows)
                            std::fprintf(
                                report, "display %u suggestion '%s' %.1f,%.1f..%.1f,%.1f%%\n",
                                entry.display_id, suggestion.label.c_str(),
                                suggestion.region.left_percent, suggestion.region.top_percent,
                                suggestion.region.right_percent, suggestion.region.bottom_percent);
                    std::fclose(report);
                }
                worker.WithLatestFrame([&](const FrameView& view) {
                    std::FILE* image = OpenDebugFile("/tmp/sidescopes-frame.ppm", "wb");
                    if (!image) return;
                    std::fprintf(image, "P6\n%d %d\n255\n", view.width / 2, view.height / 2);
                    for (int py = 0; py < view.height - 1; py += 2) {
                        for (int px = 0; px < view.width - 1; px += 2) {
                            const Color color = view.ColorAt(px, py);
                            std::fputc(color.r, image);
                            std::fputc(color.g, image);
                            std::fputc(color.b, image);
                        }
                    }
                    std::fclose(image);
                });
            }
            if (BeginRegionPick(picker_displays, *want_region_pick)) region_picking = true;
            last_activity = glfwGetTime();
        }

        // The region border is live: dragging its edges, corners, or move
        // tab adjusts the region with the scopes following along.
        if (!region_picking) {
            const RegionBorderEdit edit = PollRegionBorderEdit();
            if (edit.dismissed) {
                // The border's own close affordances mean "stop tracking
                // this region"; full screen is the fallback.
                reset_region_to_full();
                last_activity = glfwGetTime();
            } else if (edit.region) {
                analysis.region = *edit.region;
                // The analysis-dirty path below syncs the border this same
                // iteration; a second sync here would double the border
                // work on every drag step.
                analysis_dirty = true;
                last_activity = glfwGetTime();
            }
        }

        // While the picker is up, whatever the user indicates previews on
        // the scopes immediately; confirmation keeps it, Esc restores.
        if (region_picking) {
            const RegionPickPoll poll = PollRegionPick();
            const auto apply_region = [&](const RegionOfInterest& region) {
                if (region.left_percent == analysis.region.left_percent &&
                    region.top_percent == analysis.region.top_percent &&
                    region.right_percent == analysis.region.right_percent &&
                    region.bottom_percent == analysis.region.bottom_percent)
                    return;
                analysis.region = region;
                analysis_dirty = true;
                last_activity = glfwGetTime();
            };
            // Live preview only for the tracked display: previewing a
            // suggestion on another display would mean flapping the
            // capture stream on every hover. The switch happens once, on
            // confirmation.
            if (poll.preview && poll.display_id == capture_display) apply_region(*poll.preview);
            if (poll.finished || !poll.active) {
                region_picking = false;
                if (poll.confirmed) {
                    if (poll.display_id != 0 && poll.display_id != capture_display) {
                        desired_display = poll.display_id;
                        capture->Stop();
                        if (!start_capture()) {
                            capture_dead.store(true);
                            next_capture_retry = glfwGetTime() + 2.0;
                        }
                    }
                    apply_region(*poll.confirmed);
                } else {
                    // Cancelled: reset all drawing, pending and confirmed.
                    // Full region means capture snaps back to the display
                    // this window sits on.
                    apply_region(RegionOfInterest{});
                }
                sync_region_border();
                last_activity = glfwGetTime();
            }
        }

        if (analysis_dirty) {
            worker.UpdateSettings(analysis);
            projection_vectorscope.Configure(analysis.vectorscope);
            projection_waveform.Configure(analysis.waveform);
            sync_region_border();
            analysis_dirty = false;
            last_activity = glfwGetTime();
            next_preferences_save = glfwGetTime() + 1.0;
        }
        if (next_preferences_save > 0.0 && glfwGetTime() > next_preferences_save) {
            save_preferences();
            next_preferences_save = -1.0;
        }
    }

    save_preferences();
    HideRegionBorder();
    worker.Stop();
    capture->Stop();
    graphics->Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
