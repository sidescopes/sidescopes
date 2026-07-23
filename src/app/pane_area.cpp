#include "app/pane_area.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/capture_controller.h"
#include "app/color_readout.h"
#include "app/overlay_render.h"
#include "app/param_menu.h"
#include "app/scope_layout.h"
#include "app/status_bar.h"
#include "core/trace_intensity.h"
#include "imgui.h"
#include "platform/desktop.h"

namespace sidescopes {
namespace {

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

}  // namespace

PaneArea::PaneArea(const PaneAreaContext& context, std::map<std::string, ScopeInstance> projections,
                   ScopeTextureSet textures)
    : m_graphics(context.graphics),
      m_view(context.view),
      m_registry(context.registry),
      m_analysis(context.analysis),
      m_output(context.output),
      m_capture(context.capture),
      m_pins(context.pins),
      m_projections(std::move(projections)),
      m_scopeTextures(std::move(textures.textures)),
      m_panePoints(std::move(textures.panePoints)),
      m_paneIds(std::move(textures.paneIds)),
      m_dividerIds(std::move(textures.dividerIds))
{
}

PaneRenderOutcome PaneArea::draw(const PaneRenderInput& input)
{
    // The enabled scopes stack in activation order along the chosen axis, each
    // pane sized by its weight. A scope's pane point and rect live at its own
    // identity index, so the adaptive block and the context menu read back the
    // right pane.
    Pass pass{input, PaneRenderOutcome{}};
    m_paneRects.assign(m_registry.scopes().size(), ImVec4());
    // One reservation around every pane path, help pages included: whatever
    // fills the area lives in a child sized to leave the status bar's strip
    // below it, so the bar keeps the foot of the window in every state and a
    // centred help block centres on the space it actually has. The host window
    // carries no scrollbar, and neither does this.
    ImGui::BeginChild("##pane-area", ImVec2(0.0f, -statusBarHeight()), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawContent(pass);
    ImGui::EndChild();

    return pass.outcome;
}

void PaneArea::drawContent(Pass& pass)
{
    if (!m_capture.permissionGranted()) {
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
    if (m_capture.dead()) {
        const std::string status = m_capture.status();
        drawCaptureHelp("Screen capture was interrupted", {status, "Reconnecting automatically..."}, false);

        return;
    }
    const std::vector<std::string>& stack = m_view.stack().ids();
    if (stack.size() == 1) {
        drawScopeById(stack.front(), pass);
    } else if (stack.size() > 1) {
        drawStack(pass);
    }
}

void PaneArea::drawStack(Pass& pass)
{
    // Weights split the axis; a divider between each neighboring pair is a thin
    // grab strip that resizes them. Item spacing is zeroed so panes and dividers
    // tile the area exactly, and restored inside each pane so scope contents
    // keep their normal breathing room.
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const std::vector<std::string>& ids = m_view.stack().ids();
    const PaneLayout& layout = m_view.layout();
    const int count = static_cast<int>(ids.size());
    const float divider = DividerThickness * pass.input.uiScale;
    const std::vector<float> weights = layout.stackWeights(ids);
    const bool sideBySide = resolveSplitDirection(layout.orientation(), area.x, area.y, weights, stackAspects(),
                                                  divider) == SplitDirection::SideBySide;
    const float axisLength = (sideBySide ? area.x : area.y) - divider * static_cast<float>(count - 1);
    const std::vector<float> lengths = paneLengths(weights, axisLength, MinPaneLength * pass.input.uiScale);
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    for (int pane = 0; pane < count; ++pane) {
        const auto index = static_cast<std::size_t>(pane);
        const ImVec2 paneSize = sideBySide ? ImVec2(lengths[index], area.y) : ImVec2(area.x, lengths[index]);
        ImGui::BeginChild(m_paneIds[index].c_str(), paneSize);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
        drawScopeById(ids[index], pass);
        ImGui::PopStyleVar();
        ImGui::EndChild();
        if (pane + 1 < count) {
            if (sideBySide) {
                ImGui::SameLine();
            }
            drawDivider(pane, sideBySide, divider, area, lengths, pass);
            if (sideBySide) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::PopStyleVar();
}

std::vector<float> PaneArea::stackAspects() const
{
    // A module's own declaration wins; the host table covers the color
    // picker and modules that declare nothing.
    std::vector<float> aspects;
    const std::vector<std::string>& ids = m_view.stack().ids();
    aspects.reserve(ids.size());
    for (const std::string& scopeId : ids) {
        const HostScope* hostScope = m_registry.byId(scopeId);
        const float declared =
            hostScope != nullptr && hostScope->descriptor != nullptr ? hostScope->descriptor->preferred_aspect : 0.0f;
        aspects.push_back(declared > 0.0f ? declared : preferredScopeAspect(scopeId));
    }

    return aspects;
}

void PaneArea::drawDivider(int leftPane, bool sideBySide, float thickness, const ImVec2& area,
                           const std::vector<float>& lengths, Pass& pass)
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
        adjustDividerWeights(leftPane, sideBySide ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y, lengths,
                             pass.input.uiScale);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        equalizeDividerWeights(leftPane, pass);
    }
}

void PaneArea::paintDivider(bool sideBySide, bool highlighted)
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

void PaneArea::adjustDividerWeights(int leftPane, float deltaPixels, const std::vector<float>& lengths, float uiScale)
{
    if (deltaPixels == 0.0f) {
        return;
    }
    const auto left = static_cast<std::size_t>(leftPane);
    PaneLayout& layout = m_view.layout();
    const std::string& leftId = m_view.stack().ids()[left];
    const std::string& rightId = m_view.stack().ids()[left + 1];
    const auto [newLeft, newRight] = dragDividerWeights(layout.weight(leftId), layout.weight(rightId), lengths[left],
                                                        lengths[left + 1], deltaPixels, MinPaneLength * uiScale);
    layout.setWeight(leftId, newLeft);
    layout.setWeight(rightId, newRight);
}

void PaneArea::equalizeDividerWeights(int leftPane, Pass& pass)
{
    // Double-click reset: the two neighbors share their combined weight evenly,
    // leaving every other pane untouched.
    const auto left = static_cast<std::size_t>(leftPane);
    PaneLayout& layout = m_view.layout();
    const std::string& leftId = m_view.stack().ids()[left];
    const std::string& rightId = m_view.stack().ids()[left + 1];
    const float average = (layout.weight(leftId) + layout.weight(rightId)) * 0.5f;
    layout.setWeight(leftId, average);
    layout.setWeight(rightId, average);
    pass.outcome.preferencesSaveDue = true;
    pass.outcome.activity = true;
}

void PaneArea::drawScopeById(std::string_view id, Pass& pass)
{
    const auto index = static_cast<std::size_t>(m_registry.indexOf(id));
    m_panePoints[index] = ImGui::GetContentRegionAvail();
    const ImVec2 paneMin = ImGui::GetCursorScreenPos();
    const ImVec2 paneAvail = ImGui::GetContentRegionAvail();
    m_paneRects[index] = ImVec4(paneMin.x, paneMin.y, paneMin.x + paneAvail.x, paneMin.y + paneAvail.y);
    if (id == VectorscopeScopeId) {
        drawVectorscopePane(pass);
    } else if (id == HistogramScopeId) {
        const ScopeInstance* instance = projectionFor(HistogramScopeId);
        if (instance != nullptr) {
            drawHistogram(textureForId(HistogramScopeId), m_output, *instance, histogramStyle(), m_view.graticule(),
                          pass.input.vectorscopeColor, m_histogramScratch);
        }
    } else if (id == ColorPickerScopeId) {
        drawColorPicker(pass.input.vectorscopeColor, m_pins, pass.input.monospaceFont);
    } else {
        drawWaveformPane(id, pass);
    }
}

void PaneArea::drawVectorscopePane(Pass& pass)
{
    const DrawnScope scope = drawScopeImage(textureForId(VectorscopeScopeId), true, static_cast<float>(m_view.zoom()));
    const SsParamInfo* gain = firstParamOfKind(descriptorFor(VectorscopeScopeId), SS_PARAM_INTENSITY);
    TraceParams& traces = m_view.traces();
    if (gain != nullptr) {
        if (const auto adjusted = traceIntensityGesture(scope, VectorscopeScopeId, traces.intensity(VectorscopeScopeId),
                                                        static_cast<float>(gain->default_value),
                                                        static_cast<float>(gain->intensity_shift), m_flash)) {
            traces.setIntensity(VectorscopeScopeId, adjusted->intensity);
            m_analysis.scopeParams[VectorscopeScopeId][gain->key] = adjusted->gain;
            pass.outcome.analysisDirty = true;
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
        if (pass.input.vectorscopeColor) {
            drawMarkers(scope, instance->markers(toSsColor(*pass.input.vectorscopeColor)));
        }
    }
    draw->PopClipRect();
    if (m_view.zoom() > 1) {
        char badge[4] = {static_cast<char>('0' + m_view.zoom()), 'x', '\0'};
        draw->AddText(ImVec2(scope.origin.x + scope.size.x - 26, scope.origin.y + 6), GraticuleLabel, badge);
    }
}

void PaneArea::drawWaveformPane(std::string_view id, Pass& pass)
{
    // The waveform and its parade share one intensity control; each draws its
    // own instance's scale and cursor markers, and the module's marker layout
    // already follows its configured mode, so the host needs no branch.
    const DrawnScope scope = drawScopeImage(textureForId(id), false);
    const SsParamInfo* gain = firstParamOfKind(descriptorFor(WaveformScopeId), SS_PARAM_INTENSITY);
    TraceParams& traces = m_view.traces();
    if (gain != nullptr) {
        if (const auto adjusted = traceIntensityGesture(scope, WaveformScopeId, traces.intensity(WaveformScopeId),
                                                        static_cast<float>(gain->default_value),
                                                        static_cast<float>(gain->intensity_shift), m_flash)) {
            traces.setIntensity(WaveformScopeId, adjusted->intensity);
            setWaveformGain(adjusted->gain);
            pass.outcome.analysisDirty = true;
        }
    }
    const ScopeInstance* instance = projectionFor(id);
    if (instance != nullptr) {
        if (m_view.graticule()) {
            drawGraticule(scope, instance->graticule(), GraticuleStyle{});
        }
        if (pass.input.waveformColor) {
            drawMarkers(scope, instance->markers(toSsColor(*pass.input.waveformColor)));
        }
    }
}

const SsScopeDescriptor* PaneArea::descriptorFor(std::string_view id) const
{
    const HostScope* hostScope = m_registry.byId(id);

    return hostScope != nullptr ? hostScope->descriptor : nullptr;
}

const ScopeInstance* PaneArea::projectionFor(std::string_view id) const
{
    const auto at = m_projections.find(std::string{id});

    return at != m_projections.end() ? &at->second : nullptr;
}

void PaneArea::configureProjections()
{
    // Reconfigures every projection instance from the current settings, through
    // the same assembleScopeParams the worker uses, so an overlay can never
    // disagree with its trace.
    for (auto& [id, instance] : m_projections) {
        const SsScopeDescriptor* descriptor = descriptorFor(id);
        std::vector<SsParamValue> values;
        const auto params = m_analysis.scopeParams.find(id);
        if (params != m_analysis.scopeParams.end() && descriptor != nullptr) {
            values = assembleScopeParams(params->second, *descriptor);
        }
        (void)instance.configure(values);
    }
}

HistogramStyle PaneArea::histogramStyle() const
{
    const auto scope = m_analysis.scopeParams.find(HistogramScopeId);
    if (scope == m_analysis.scopeParams.end()) {
        return HistogramStyle::PerChannel;
    }
    const auto style = scope->second.find("style");
    const double value = style != scope->second.end() ? style->second : 0.0;

    return value < 0.5 ? HistogramStyle::PerChannel : HistogramStyle::Combined;
}

void PaneArea::setWaveformGain(double gain)
{
    m_analysis.scopeParams[WaveformScopeId]["gain"] = gain;
    m_analysis.scopeParams[ParadeScopeId]["gain"] = gain;
}

const ScopeImage& PaneArea::imageFor(std::string_view id) const
{
    static const ScopeImage empty;
    const auto at = m_output.images.find(std::string{id});

    return at != m_output.images.end() ? at->second : empty;
}

bool PaneArea::hasTexture(std::string_view id) const
{
    return m_scopeTextures.find(std::string{id}) != m_scopeTextures.end();
}

ScopeTexture& PaneArea::textureForId(std::string_view id)
{
    return *m_scopeTextures.at(std::string{id});
}

void PaneArea::uploadScope(std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image)
{
    if (image.width <= 0 || image.height <= 0) {
        return;
    }
    if (image.rgba.size() < static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4) {
        return;
    }
    if (texture->width() != image.width || texture->height() != image.height) {
        texture = m_graphics.createScopeTexture(image.width, image.height);
    }
    texture->upload(image);
}

void PaneArea::uploadVisibleScopes()
{
    for (const std::string& id : m_view.stack().ids()) {
        const auto texture = m_scopeTextures.find(id);
        if (texture == m_scopeTextures.end()) {
            continue;  // the color picker has no texture
        }
        uploadScope(texture->second, imageFor(id));
    }
}

ImVec2 PaneArea::paneSizePoints(std::string_view id) const
{
    return m_panePoints[static_cast<std::size_t>(m_registry.indexOf(id))];
}

int PaneArea::paneAt(const ImVec2& point) const
{
    // Which pane the click landed in decides which options lead the menu.
    int clickedPane = -1;
    for (std::size_t pane = 0; pane < m_paneRects.size(); ++pane) {
        const ImVec4& rect = m_paneRects[pane];
        if (rect.z <= rect.x || rect.w <= rect.y) {
            continue;
        }
        if (point.x >= rect.x && point.x < rect.z && point.y >= rect.y && point.y < rect.w) {
            clickedPane = static_cast<int>(pane);
        }
    }

    return clickedPane;
}

}  // namespace sidescopes
