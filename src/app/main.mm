// The SideScopes application shell: a compact, always-on-top window showing
// one scope at a time. All analysis lives in the core library on its own
// thread; this file owns the window, the Metal textures, the interaction
// model (gestures, native menu, region selection), and preferences.

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>

#include "core/analysis_worker.h"
#include "core/frame_mailbox.h"
#include "core/marker_smoother.h"
#include "core/photo_region_detector.h"
#include "core/preferences.h"
#include "core/region_suggestions.h"
#include "core/scopes/graticule.h"
#include "core/trace_intensity.h"
#include "platform/desktop.h"
#include "platform/native_menu.h"
#include "platform/region_selection.h"
#include "platform/screen_capture.h"

namespace {

using namespace sidescopes;

enum class ViewMode { Vectorscope, Waveform, Both, Histogram };

enum MenuAction {
    kMenuShowVectorscope = 1,
    kMenuShowWaveform,
    kMenuShowBoth,
    kMenuShowHistogram,
    kMenuWaveformLuma = 10,
    kMenuWaveformRgb,
    kMenuWaveformRgbAndLuma,
    kMenuWaveformParade,
    kMenuMatrixBt601 = 20,
    kMenuMatrixBt709,
    kMenuSelectRegion = 30,
    kMenuFullScreenRegion,
    kMenuToggleGraticule = 40,
    kMenuTogglePercentValues,
    kMenuOpenSettings = 50,
    kMenuQuit,
};

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

// A texture the CPU-side scope images are uploaded into every time the
// analysis worker publishes a new version.
struct ScopeTexture {
    ScopeTexture(id<MTLDevice> device, int width, int height) : width_(width), height_(height) {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeManaged;
        texture_ = [device newTextureWithDescriptor:descriptor];
    }

    void Upload(const ScopeImage& image) {
        [texture_ replaceRegion:MTLRegionMake2D(0, 0, width_, height_)
                    mipmapLevel:0
                      withBytes:image.rgba.data()
                    bytesPerRow:static_cast<NSUInteger>(width_) * 4];
    }

    [[nodiscard]] ImTextureID Id() const {
        return reinterpret_cast<ImTextureID>((__bridge void*)texture_);
    }

    int width_;
    int height_;
    id<MTLTexture> texture_;
};

struct DrawnScope {
    ImVec2 origin;
    ImVec2 size;
};

// Draws a scope image into the available space. The vectorscope keeps its
// square aspect (it is a polar plot); the waveform stretches, because its
// horizontal axis is arbitrary image columns.
DrawnScope DrawScopeImage(const ScopeTexture& texture, bool keep_aspect) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImVec2 size = available;
    if (keep_aspect) {
        const float scale =
            std::max(0.05f, std::min(available.x / static_cast<float>(texture.width_),
                                     available.y / static_cast<float>(texture.height_)));
        size = ImVec2(texture.width_ * scale, texture.height_ * scale);
    }
    ImVec2 cursor = ImGui::GetCursorPos();
    cursor.x += std::max(0.0f, (available.x - size.x) * 0.5f);
    cursor.y += std::max(0.0f, (available.y - size.y) * 0.5f);
    ImGui::SetCursorPos(cursor);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(texture.Id(), size);
    return DrawnScope{origin, size};
}

ImVec2 At(const DrawnScope& scope, const NormalizedPoint& point) {
    return ImVec2(scope.origin.x + point.x * scope.size.x, scope.origin.y + point.y * scope.size.y);
}

void DrawVectorscopeOverlay(const DrawnScope& scope, const VectorscopeGraticule& graticule) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto stroke_color = [](GraticuleStroke stroke) -> ImU32 {
        switch (stroke) {
            case GraticuleStroke::GridMajor:
                return IM_COL32(150, 150, 150, 90);
            case GraticuleStroke::Accent:
                return IM_COL32(210, 165, 70, 130);
            case GraticuleStroke::SkinTone:
                return IM_COL32(230, 170, 140, 150);
            case GraticuleStroke::Grid:
                break;
        }
        return IM_COL32(120, 120, 120, 45);
    };

    for (const GraticuleLine& line : graticule.lines) {
        draw->AddLine(At(scope, line.from), At(scope, line.to), stroke_color(line.stroke),
                      line.stroke == GraticuleStroke::GridMajor ? 1.5f : 1.0f);
    }
    for (const GraticuleCircle& circle : graticule.circles) {
        draw->AddCircle(At(scope, circle.center), circle.radius * scope.size.x,
                        stroke_color(circle.stroke), 64);
    }
    const ImU32 target_color = IM_COL32(210, 165, 70, 130);
    for (const GraticuleTarget& target : graticule.targets) {
        const ImVec2 center = At(scope, target.center);
        const float box = target.primary ? 5.0f : 3.0f;
        draw->AddRect(ImVec2(center.x - box, center.y - box),
                      ImVec2(center.x + box, center.y + box), target_color);
        if (!target.label.empty())
            draw->AddText(ImVec2(center.x + 7, center.y - 7), target_color, target.label.c_str());
    }
}

void DrawWaveformOverlay(const DrawnScope& scope) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool roomy = scope.size.y >= 140.0f;
    for (const WaveformScaleLine& line : BuildWaveformScale()) {
        const float y = scope.origin.y + line.y * scope.size.y;
        const ImU32 color = line.major ? IM_COL32(170, 170, 170, 140) : IM_COL32(150, 150, 150, 85);
        draw->AddLine(ImVec2(scope.origin.x, y), ImVec2(scope.origin.x + scope.size.x, y), color);
        if (line.major || roomy)
            draw->AddText(ImVec2(scope.origin.x + 4, y + 1), IM_COL32(190, 190, 190, 170),
                          line.label.c_str());
    }
}

void DrawPointMarker(const DrawnScope& scope, const NormalizedPoint& point, ImU32 color) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center = At(scope, point);
    draw->AddCircle(center, 5.0f, color, 0, 2.0f);
    draw->AddCircle(center, 6.5f, IM_COL32(0, 0, 0, 200), 0, 1.0f);
}

void DrawLevelMarker(const DrawnScope& scope, float normalized_y, ImU32 color) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float y = scope.origin.y + normalized_y * scope.size.y;
    draw->AddLine(ImVec2(scope.origin.x, y), ImVec2(scope.origin.x + scope.size.x, y), color, 1.5f);
}

// Toolbar icon buttons drawn with the draw list: corner brackets for region
// selection, an outline rectangle for full screen.
bool IconButton(const char* id, bool brackets, const char* tooltip) {
    const float height = ImGui::GetTextLineHeight() + 4.0f;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(height + 8.0f, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered())
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    const ImVec2 a(min.x + 7, min.y + 3);
    const ImVec2 b(max.x - 7, max.y - 3);
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    const float stroke = 1.5f;
    if (brackets) {
        const float length = std::min(b.x - a.x, b.y - a.y) * 0.4f;
        const auto corner = [&](float cx, float cy, float dx, float dy) {
            draw->AddLine(ImVec2(cx, cy), ImVec2(cx + dx * length, cy), color, stroke);
            draw->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + dy * length), color, stroke);
        };
        corner(a.x, a.y, 1, 1);
        corner(b.x, a.y, -1, 1);
        corner(a.x, b.y, 1, -1);
        corner(b.x, b.y, -1, -1);
    } else {
        draw->AddRect(a, b, color, 1.0f, 0, stroke);
    }
    ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
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
    for (const char* path :
         {"/System/Library/Fonts/HelveticaNeue.ttc", "/System/Library/Fonts/SFNS.ttf",
          "/System/Library/Fonts/Supplemental/Arial.ttf"}) {
        if (io.Fonts->AddFontFromFileTTF(path, 13.0f, &config)) return;
    }
}

}  // namespace

int main() {
    if (!glfwInit()) return 1;

    const Preferences startup = LoadPreferences(PreferencesFilePath());

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(startup.window_width, startup.window_height, "SideScopes",
                                          nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    if (startup.window_x >= 0) glfwSetWindowPos(window, startup.window_x, startup.window_y);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> command_queue = [device newCommandQueue];

    NSWindow* native_window = glfwGetCocoaWindow(window);
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    native_window.contentView.layer = layer;
    native_window.contentView.wantsLayer = YES;
    // Above document and panel windows (Quick Look previews float higher
    // than ordinary floating windows), on every Space.
    native_window.level = NSStatusWindowLevel;
    native_window.collectionBehavior =
        NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is ours to persist
    ImGui::StyleColorsDark();
    ApplyTheme();
    LoadInterfaceFont(window);
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplMetal_Init(device);

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
    const bool permission_granted = capture->RequestPermission() == CapturePermission::Granted;
    const auto start_capture = [&]() -> bool {
        const auto targets = capture->ListTargets();
        if (targets.empty()) return false;
        if (!capture->Start(targets.front(), 30, mailbox)) return false;
        capture_display = static_cast<uint32_t>(std::stoul(targets.front().identifier));
        {
            std::lock_guard lock(status_mutex);
            capture_status = "capturing " + targets.front().description;
        }
        capture_dead.store(false);
        return true;
    };
    if (permission_granted) {
        start_capture();
    } else {
        std::lock_guard lock(status_mutex);
        capture_status = "screen recording permission missing - grant it in System Settings and "
                         "relaunch";
    }

    // --- state, seeded from preferences ---
    AnalysisSettings analysis;
    analysis.vectorscope.gain = startup.vectorscope_gain;
    analysis.vectorscope.sampling_stride = startup.vectorscope_stride;
    analysis.vectorscope.matrix = startup.matrix;
    analysis.waveform.gain = startup.waveform_gain;
    analysis.waveform.sampling_stride = startup.waveform_stride;
    analysis.waveform.mode = startup.waveform_mode;
    analysis.histogram.gain = startup.histogram_gain;
    analysis.histogram.sampling_stride = startup.histogram_stride;
    analysis.region = startup.region;
    bool analysis_dirty = true;

    float vectorscope_intensity = IntensityFromTraceGain(analysis.vectorscope.gain);
    float waveform_intensity = IntensityFromTraceGain(analysis.waveform.gain);
    float histogram_intensity = IntensityFromTraceGain(analysis.histogram.gain);
    float vectorscope_smoothing_ms = startup.vectorscope_smoothing_ms;
    float waveform_smoothing_ms = startup.waveform_smoothing_ms;
    auto view_mode = static_cast<ViewMode>(std::clamp(startup.view_mode, 0, 3));
    bool show_graticule = startup.show_graticule;
    bool values_as_percent = startup.values_as_percent;
    bool show_settings = false;

    // Projection-only engine instances kept in sync with the analysis
    // settings; they never accumulate, they only place overlays and markers.
    Vectorscope projection_vectorscope;
    Waveform projection_waveform;

    MarkerSmoother vectorscope_marker;
    MarkerSmoother waveform_marker;

    ScopeTexture vectorscope_texture(device, Vectorscope::kSize, Vectorscope::kSize);
    ScopeTexture waveform_texture(device, Waveform::kColumns, Waveform::kLevels);
    ScopeTexture histogram_texture(device, Histogram::kBins, Histogram::kHeight);

    worker.Start();

    uint64_t output_version = 0;
    AnalysisWorker::Output output;
    double last_activity = glfwGetTime();
    double next_capture_retry = 0.0;
    double intensity_flash_until = 0.0;
    double next_preferences_save = -1.0;
    DesktopPoint last_cursor{-1.0, -1.0};

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
    const auto save_preferences = [&] {
        Preferences preferences;
        preferences.vectorscope_gain = analysis.vectorscope.gain;
        preferences.waveform_gain = analysis.waveform.gain;
        preferences.vectorscope_stride = analysis.vectorscope.sampling_stride;
        preferences.waveform_stride = analysis.waveform.sampling_stride;
        preferences.vectorscope_smoothing_ms = vectorscope_smoothing_ms;
        preferences.waveform_smoothing_ms = waveform_smoothing_ms;
        preferences.matrix = analysis.vectorscope.matrix;
        preferences.waveform_mode = analysis.waveform.mode;
        preferences.histogram_gain = analysis.histogram.gain;
        preferences.histogram_stride = analysis.histogram.sampling_stride;
        preferences.region = analysis.region;
        preferences.view_mode = static_cast<int>(view_mode);
        preferences.show_graticule = show_graticule;
        preferences.values_as_percent = values_as_percent;
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
        if (permission_granted && capture_dead.load() && glfwGetTime() > next_capture_retry) {
            capture->Stop();
            if (start_capture())
                last_activity = glfwGetTime();
            else
                next_capture_retry = glfwGetTime() + 2.0;
        }

        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        if (framebuffer_width == 0 || framebuffer_height == 0) continue;
        layer.drawableSize = CGSizeMake(framebuffer_width, framebuffer_height);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) continue;

        if (worker.FetchOutput(output_version, output)) {
            vectorscope_texture.Upload(output.vectorscope_image);
            waveform_texture.Upload(output.waveform_image);
            histogram_texture.Upload(output.histogram_image);
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
                const IntRect self_window{
                    static_cast<int>((window_x - geometry->origin_x - 8) * scale_x),
                    static_cast<int>((window_y - geometry->origin_y - 42) * scale_y),
                    static_cast<int>((window_w + 16) * scale_x),
                    static_cast<int>((window_h + 58) * scale_y)};
                if (self_window.x != analysis.masked_window.x ||
                    self_window.y != analysis.masked_window.y ||
                    self_window.width != analysis.masked_window.width ||
                    self_window.height != analysis.masked_window.height) {
                    analysis.masked_window = self_window;
                    analysis_dirty = true;
                }
            }
        }

        // Cursor color, smoothed per scope with its own rhythm.
        std::optional<FloatColor> vectorscope_color;
        std::optional<FloatColor> waveform_color;
        if (!capture_dead.load() && frame_size && capture_display != 0) {
            if (const auto cursor = GlobalCursorPosition()) {
                if (std::abs(cursor->x - last_cursor.x) + std::abs(cursor->y - last_cursor.y) >
                    0.5) {
                    last_cursor = *cursor;
                    last_activity = glfwGetTime();
                }
                if (const auto geometry = GeometryOfDisplay(capture_display)) {
                    const int pixel_x =
                        static_cast<int>((cursor->x - geometry->origin_x) * frame_size->width /
                                         geometry->width_points);
                    const int pixel_y =
                        static_cast<int>((cursor->y - geometry->origin_y) * frame_size->height /
                                         geometry->height_points);
                    if (const auto sampled = worker.SampleFrameColor(pixel_x, pixel_y)) {
                        vectorscope_marker.SetTimeConstant(vectorscope_smoothing_ms);
                        waveform_marker.SetTimeConstant(waveform_smoothing_ms);
                        vectorscope_color = vectorscope_marker.Update(*sampled, io.DeltaTime);
                        waveform_color = waveform_marker.Update(*sampled, io.DeltaTime);
                    }
                }
            }
        }

        // --- frame ---
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

        id<MTLCommandBuffer> commands = [command_queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];

        ImGui_ImplMetal_NewFrame(pass);
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        ImGui::Begin("##host", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

        // Toolbar: view switcher, region tools, cursor readout.
        const auto mode_button = [&](const char* label, ViewMode mode) {
            const bool active = view_mode == mode;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(label)) view_mode = mode;
            if (active) ImGui::PopStyleColor();
            ImGui::SameLine();
        };
        mode_button("Vector", ViewMode::Vectorscope);
        mode_button("Wave", ViewMode::Waveform);
        mode_button("Both", ViewMode::Both);
        mode_button("Histo", ViewMode::Histogram);

        // Keyboard shortcuts mirror the toolbar and region tools.
        bool want_region_pick_hotkey = false;
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) view_mode = ViewMode::Vectorscope;
            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) view_mode = ViewMode::Waveform;
            if (ImGui::IsKeyPressed(ImGuiKey_B, false)) view_mode = ViewMode::Both;
            if (ImGui::IsKeyPressed(ImGuiKey_H, false)) view_mode = ViewMode::Histogram;
            if (ImGui::IsKeyPressed(ImGuiKey_A, false)) want_region_pick_hotkey = true;
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                analysis.region = RegionOfInterest{};
                analysis_dirty = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) show_graticule = !show_graticule;
        }

        bool want_region_pick = want_region_pick_hotkey;
        if (IconButton("##select-region", true, "Select scoped area (drag on screen, A)"))
            want_region_pick = true;
        ImGui::SameLine(0.0f, 2.0f);
        if (!is_full_region()) {
            if (IconButton("##full-region", false, "Reset to full screen")) {
                analysis.region = RegionOfInterest{};
                analysis_dirty = true;
            }
            ImGui::SameLine(0.0f, 2.0f);
        }

        if (vectorscope_color) {
            const FloatColor& color = *vectorscope_color;
            char readout[48];
            if (values_as_percent)
                std::snprintf(readout, sizeof(readout), "%2.0f%% %2.0f%% %2.0f%%", color.r / 2.55,
                              color.g / 2.55, color.b / 2.55);
            else
                std::snprintf(readout, sizeof(readout), "%3.0f %3.0f %3.0f", color.r, color.g,
                              color.b);
            const float text_width = ImGui::CalcTextSize(readout).x;
            const float swatch = ImGui::GetTextLineHeight();
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - (text_width + swatch + 6));
            ImGui::ColorButton("##cursor-color",
                               ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(swatch, swatch));
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextUnformatted(readout);
        } else {
            ImGui::NewLine();
        }

        // Scroll over a scope adjusts its intensity; double-click resets.
        static float* flash_intensity = nullptr;
        const auto scope_gestures = [&](const DrawnScope& scope, float& intensity, float& gain,
                                        float default_gain) {
            if (!ImGui::IsItemHovered()) return;
            if (io.MouseWheel != 0.0f) {
                intensity = std::clamp(intensity + 2.0f * io.MouseWheel, 0.0f, 100.0f);
                gain = TraceGainFromIntensity(intensity);
                analysis_dirty = true;
                intensity_flash_until = glfwGetTime() + 1.2;
                flash_intensity = &intensity;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                gain = default_gain;
                intensity = IntensityFromTraceGain(gain);
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
            const DrawnScope scope = DrawScopeImage(vectorscope_texture, true);
            scope_gestures(scope, vectorscope_intensity, analysis.vectorscope.gain, 3.0f);
            if (show_graticule)
                DrawVectorscopeOverlay(scope, BuildVectorscopeGraticule(projection_vectorscope));
            if (vectorscope_color) {
                if (const auto point = projection_vectorscope.Project(*vectorscope_color))
                    DrawPointMarker(scope, *point, IM_COL32(255, 255, 255, 255));
            }
        };
        const auto draw_waveform = [&] {
            const DrawnScope scope = DrawScopeImage(waveform_texture, false);
            scope_gestures(scope, waveform_intensity, analysis.waveform.gain, 0.05f);
            if (show_graticule) DrawWaveformOverlay(scope);
            if (waveform_color) {
                if (analysis.waveform.mode == WaveformMode::Luma) {
                    if (const auto point = projection_waveform.Project(*waveform_color))
                        DrawLevelMarker(scope, point->y, IM_COL32(255, 220, 80, 220));
                } else {
                    const float channels[3] = {waveform_color->r, waveform_color->g,
                                               waveform_color->b};
                    const ImU32 colors[3] = {IM_COL32(255, 90, 90, 230), IM_COL32(90, 255, 90, 230),
                                             IM_COL32(110, 110, 255, 230)};
                    for (int channel = 0; channel < 3; ++channel)
                        DrawLevelMarker(scope, (255.0f - channels[channel]) / 255.0f,
                                        colors[channel]);
                }
            }
        };

        const auto draw_histogram = [&] {
            const DrawnScope scope = DrawScopeImage(histogram_texture, false);
            scope_gestures(scope, histogram_intensity, analysis.histogram.gain, 1.0f);
            if (show_graticule) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                for (int quarter = 0; quarter <= 4; ++quarter) {
                    const float x = scope.origin.x + scope.size.x * quarter / 4.0f;
                    draw->AddLine(ImVec2(x, scope.origin.y),
                                  ImVec2(x, scope.origin.y + scope.size.y),
                                  IM_COL32(150, 150, 150, 70));
                }
            }
            if (vectorscope_color) {
                const float channels[3] = {vectorscope_color->r, vectorscope_color->g,
                                           vectorscope_color->b};
                const ImU32 colors[3] = {IM_COL32(255, 90, 90, 230), IM_COL32(90, 255, 90, 230),
                                         IM_COL32(110, 110, 255, 230)};
                ImDrawList* draw = ImGui::GetWindowDrawList();
                for (int channel = 0; channel < 3; ++channel) {
                    const float x = scope.origin.x + channels[channel] / 255.0f * scope.size.x;
                    draw->AddLine(ImVec2(x, scope.origin.y),
                                  ImVec2(x, scope.origin.y + scope.size.y), colors[channel], 1.5f);
                }
            }
        };

        const ImVec2 area = ImGui::GetContentRegionAvail();
        if (view_mode == ViewMode::Vectorscope) {
            draw_vectorscope();
        } else if (view_mode == ViewMode::Waveform) {
            draw_waveform();
        } else if (view_mode == ViewMode::Histogram) {
            draw_histogram();
        } else {
            const bool horizontal = area.x >= area.y;
            const ImVec2 half =
                horizontal ? ImVec2(area.x * 0.5f - 3, area.y) : ImVec2(area.x, area.y * 0.5f - 3);
            ImGui::BeginChild("##vectorscope", half);
            draw_vectorscope();
            ImGui::EndChild();
            if (horizontal) ImGui::SameLine();
            ImGui::BeginChild("##waveform", half);
            draw_waveform();
            ImGui::EndChild();
        }

        // Right-click: the native menu carries the modes and toggles.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            using Kind = NativeMenuItem::Kind;
            const auto waveform_mode = analysis.waveform.mode;
            const bool bt601 = analysis.vectorscope.matrix == ChromaMatrix::Bt601;
            std::vector<NativeMenuItem> menu{
                {Kind::Action, "Vectorscope", kMenuShowVectorscope,
                 view_mode == ViewMode::Vectorscope},
                {Kind::Action, "Waveform", kMenuShowWaveform, view_mode == ViewMode::Waveform},
                {Kind::Action, "Both", kMenuShowBoth, view_mode == ViewMode::Both},
                {Kind::Action, "Histogram", kMenuShowHistogram, view_mode == ViewMode::Histogram},
                {Kind::Separator, "", -1, false},
                {Kind::SubmenuBegin, "Waveform Style", -1, false},
                {Kind::Action, "Luma", kMenuWaveformLuma, waveform_mode == WaveformMode::Luma},
                {Kind::Action, "RGB", kMenuWaveformRgb, waveform_mode == WaveformMode::Rgb},
                {Kind::Action, "RGB + Luma", kMenuWaveformRgbAndLuma,
                 waveform_mode == WaveformMode::RgbAndLuma},
                {Kind::Action, "RGB Parade", kMenuWaveformParade,
                 waveform_mode == WaveformMode::RgbParade},
                {Kind::SubmenuEnd, "", -1, false},
                {Kind::SubmenuBegin, "Vectorscope Matrix", -1, false},
                {Kind::Action, "BT.601", kMenuMatrixBt601, bt601},
                {Kind::Action, "BT.709", kMenuMatrixBt709, !bt601},
                {Kind::SubmenuEnd, "", -1, false},
                {Kind::Separator, "", -1, false},
                {Kind::Action, "Select Area...", kMenuSelectRegion, false},
                {Kind::Action, "Full Screen Area", kMenuFullScreenRegion, is_full_region()},
                {Kind::Separator, "", -1, false},
                {Kind::Action, "Graticule", kMenuToggleGraticule, show_graticule},
                {Kind::Action, "Cursor Values as %", kMenuTogglePercentValues, values_as_percent},
                {Kind::Separator, "", -1, false},
                {Kind::Action, "Settings...", kMenuOpenSettings, false},
                {Kind::Action, "Quit", kMenuQuit, false},
            };
            switch (ShowNativeContextMenu(menu)) {
                case kMenuShowVectorscope:
                    view_mode = ViewMode::Vectorscope;
                    break;
                case kMenuShowWaveform:
                    view_mode = ViewMode::Waveform;
                    break;
                case kMenuShowBoth:
                    view_mode = ViewMode::Both;
                    break;
                case kMenuWaveformLuma:
                    analysis.waveform.mode = WaveformMode::Luma;
                    analysis_dirty = true;
                    break;
                case kMenuWaveformRgb:
                    analysis.waveform.mode = WaveformMode::Rgb;
                    analysis_dirty = true;
                    break;
                case kMenuWaveformRgbAndLuma:
                    analysis.waveform.mode = WaveformMode::RgbAndLuma;
                    analysis_dirty = true;
                    break;
                case kMenuWaveformParade:
                    analysis.waveform.mode = WaveformMode::RgbParade;
                    analysis_dirty = true;
                    break;
                case kMenuMatrixBt601:
                    analysis.vectorscope.matrix = ChromaMatrix::Bt601;
                    analysis_dirty = true;
                    break;
                case kMenuMatrixBt709:
                    analysis.vectorscope.matrix = ChromaMatrix::Bt709;
                    analysis_dirty = true;
                    break;
                case kMenuSelectRegion:
                    want_region_pick = true;
                    break;
                case kMenuFullScreenRegion:
                    analysis.region = RegionOfInterest{};
                    analysis_dirty = true;
                    break;
                case kMenuToggleGraticule:
                    show_graticule = !show_graticule;
                    break;
                case kMenuTogglePercentValues:
                    values_as_percent = !values_as_percent;
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
                analysis.vectorscope.gain = TraceGainFromIntensity(vectorscope_intensity);
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
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commands, encoder);
        [encoder endEncoding];
        [commands presentDrawable:drawable];
        [commands commit];

        // The blocking overlay runs after the frame is submitted; capture and
        // analysis keep flowing underneath.
        if (want_region_pick && capture_display != 0) {
            HideRegionBorder();
            // Build the offer: detected photo canvases, then windows.
            std::vector<RegionCandidate> photo_candidates;
            int detect_width = 0;
            int detect_height = 0;
            worker.WithLatestFrame([&](const FrameView& view) {
                photo_candidates = DetectPhotoRegions(view);
                detect_width = view.width;
                detect_height = view.height;
            });
            std::vector<WindowRegion> window_regions;
            if (const auto geometry = GeometryOfDisplay(capture_display)) {
                // Only the frontmost handful of windows: deeper background
                // windows are things the user is not looking at, and the
                // detected photo canvas is the primary suggestion anyway.
                constexpr int kMaxWindowSuggestions = 5;
                for (const DesktopWindow& window : OnScreenWindows(capture_display)) {
                    if (static_cast<int>(window_regions.size()) >= kMaxWindowSuggestions) break;
                    WindowRegion region;
                    region.region.left_percent =
                        std::clamp((window.x - geometry->origin_x) / geometry->width_points * 100.0,
                                   0.0, 100.0);
                    region.region.top_percent = std::clamp(
                        (window.y - geometry->origin_y) / geometry->height_points * 100.0, 0.0,
                        100.0);
                    region.region.right_percent =
                        std::clamp((window.x + window.width - geometry->origin_x) /
                                       geometry->width_points * 100.0,
                                   0.0, 100.0);
                    region.region.bottom_percent =
                        std::clamp((window.y + window.height - geometry->origin_y) /
                                       geometry->height_points * 100.0,
                                   0.0, 100.0);
                    region.application = window.application;
                    window_regions.push_back(std::move(region));
                }
            }
            const auto suggestions = BuildRegionSuggestions(photo_candidates, detect_width,
                                                            detect_height, window_regions);
            if (const auto region = PickRegionOnDisplay(capture_display, suggestions)) {
                analysis.region = *region;
                analysis_dirty = true;
            }
            last_activity = glfwGetTime();
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
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
