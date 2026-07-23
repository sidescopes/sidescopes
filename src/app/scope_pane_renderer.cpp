#include "app/scope_pane_renderer.h"

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
#include "app/context_menu.h"
#include "app/imgui_ui.h"
#include "app/overlay_render.h"
#include "app/param_menu.h"
#include "app/region_picker.h"
#include "app/row_layout.h"
#include "app/scope_layout.h"
#include "core/trace_intensity.h"
#include "imgui.h"
#include "platform/desktop.h"
#include "platform/face_detection.h"

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

// The status bar's reserved height below the panes: the spacing that parts the
// strip from the panes, the tallest thing that can stand on its row, and the
// offset that centres the row between them.
float statusBarHeight()
{
    return ImGui::GetStyle().ItemSpacing.y + iconButtonHeight() + statusRowOffset();
}

}  // namespace

ScopePaneRenderer::ScopePaneRenderer(const ScopePaneContext& context, std::map<std::string, ScopeInstance> projections,
                                     ScopeTextureSet textures)
    : m_graphics(context.graphics),
      m_view(context.view),
      m_registry(context.registry),
      m_analysis(context.analysis),
      m_output(context.output),
      m_capture(context.capture),
      m_regionPicker(context.regionPicker),
      m_pins(context.pins),
      m_shortcuts(context.shortcuts),
      m_scopeShortcuts(context.scopeShortcuts),
      m_projections(std::move(projections)),
      m_scopeTextures(std::move(textures.textures)),
      m_panePoints(std::move(textures.panePoints)),
      m_paneIds(std::move(textures.paneIds)),
      m_dividerIds(std::move(textures.dividerIds))
{
}

PaneRenderOutcome ScopePaneRenderer::drawScopeToggles(bool stackModifier)
{
    // Scope toggles are letter chips; switching is the common case, so a plain
    // click shows one scope alone and Shift stacks.
    PaneRenderOutcome outcome;
    char tooltip[96];
    const auto scopeTooltip = [&](const char* name, const std::string& binding, const char* extra) {
        std::snprintf(tooltip, sizeof(tooltip), "%s - %s to switch, Shift+%s to stack%s", name, binding.c_str(),
                      binding.c_str(), extra);
        return tooltip;
    };
    for (const HostScope& scope : m_registry.scopes()) {
        if (scope.letter == 0) {
            continue;
        }
        const ScopeChrome chrome = scopeChromeFor(scope.id);
        const char letter[2] = {scope.letter, '\0'};
        if (scopeToggleButton(chrome.buttonId, letter, m_view.shows(scope.id),
                              scopeTooltip(chrome.name, bindingFor(scope.id), chrome.extra))) {
            outcome.chosenScope = ScopeChoice{scope.id, stackModifier};
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::SameLine(0.0f, 8.0f);

    return outcome;
}

void ScopePaneRenderer::placeRegionToolbox()
{
    // The brief note after an attached window closed out from under its region
    // stays on the left, by the scopes cluster, clear of the toolbox.
    if (glfwGetTime() < m_attachNoticeUntil) {
        ImGui::TextDisabled("%s", m_attachNotice.c_str());
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

PaneRenderOutcome ScopePaneRenderer::drawRegionToolIcons(const PaneRenderInput& input)
{
    PaneRenderOutcome outcome;
    char tooltip[96];
    std::snprintf(tooltip, sizeof(tooltip), "Draw a region (%s)", m_shortcuts.drawRegion.c_str());
    const int iconPx = iconPixelSize();
    placeRegionToolbox();
    if (iconButton("##draw-region", iconTextureId(Icon::Pencil, iconPx), tooltip)) {
        m_regionPicker.request(RegionPickerMode::DrawGlobal);
    }
    ImGui::SameLine(0.0f, 2.0f);
    std::snprintf(tooltip, sizeof(tooltip), "Attach to a window (%s) - click the window or draw inside it",
                  m_shortcuts.attachWindow.c_str());
    if (iconButton("##attach-window", iconTextureId(Icon::SquarePen, iconPx), tooltip)) {
        m_regionPicker.request(RegionPickerMode::AttachWindow);
    }
    ImGui::SameLine(0.0f, 2.0f);
    // The face tool sits last among the region tools, before the reset. It
    // is always available where the platform detects faces: whether any
    // face is on screen is the picker overlay's answer to give, not the
    // toolbar's.
    if (supportsFaceDetection()) {
        std::snprintf(tooltip, sizeof(tooltip), "Attach to a face (%s)", m_shortcuts.attachFace.c_str());
        if (iconButton("##attach-face", iconTextureId(Icon::User, iconPx), tooltip)) {
            m_regionPicker.request(RegionPickerMode::AttachFace);
        }
        ImGui::SameLine(0.0f, 2.0f);
    }
    const bool fullAlready = input.regionIsFullScreen;
    if (iconButton("##full-screen", iconTextureId(Icon::Expand, iconPx),
                   fullAlready ? "Reset to full screen (Esc) - already full" : "Reset to full screen (Esc)",
                   fullAlready) &&
        !fullAlready) {
        outcome.resetToFullScreen = true;
    }
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::NewLine();

    return outcome;
}

void ScopePaneRenderer::drawStatusBar(const PaneRenderInput& input)
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
    drawPinTool(input.pinsAvailable);
    drawCursorReadout(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x, input.vectorscopeColor);
}

// The tool that samples a colour sits beside the colour it samples, not among
// the region tools - those choose what is captured, this one reads it.
void ScopePaneRenderer::drawPinTool(bool pinsAvailable)
{
    char tooltip[160];
    std::snprintf(tooltip, sizeof(tooltip), "Pin a color (%s)%s", m_shortcuts.pinColor.c_str(),
                  pinsAvailable ? " - Shift+click a color to pin several" : " - needs a scope that takes pins");
    if (iconButton("##pin-color", iconTextureId(Icon::Pipette, iconPixelSize()), tooltip, !pinsAvailable) &&
        pinsAvailable) {
        m_regionPicker.request(RegionPickerMode::PinColor);
    }
}

void ScopePaneRenderer::drawCursorReadout(float taken, const std::optional<FloatColor>& color)
{
    // The colour under the cursor, laid out inwards from its corner: the
    // swatch first, then a named percentage per channel in fixed columns, so
    // no digit coming or going moves anything. The swatch outranks the numbers
    // when the strip runs short, and both give way to whatever already stands
    // on the row.
    if (!color) {
        return;
    }
    const FloatColor& live = *color;
    const float swatch = ImGui::GetTextLineHeight();
    const float swatchStart = ImGui::GetWindowContentRegionMax().x - swatch;
    if (swatchStart < taken + 8.0f) {
        return;
    }
    const ReadoutColumns columns = measureReadoutColumns();
    const float channelsStart = swatchStart - 6.0f - columns.width;
    if (channelsStart >= taken + 8.0f) {
        drawReadoutChannels(live, channelsStart, columns);
    }
    ImGui::SameLine(swatchStart);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + rowTextDrop());
    ImGui::ColorButton("##cursor-color", ImVec4(live.r / 255.0f, live.g / 255.0f, live.b / 255.0f, 1.0f),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(swatch, swatch));
}

void ScopePaneRenderer::setStatus(std::string message)
{
    m_statusMessage = std::move(message);
    m_statusUntil = glfwGetTime() + 2.0;
}

void ScopePaneRenderer::showAttachNotice(std::string message)
{
    m_attachNotice = std::move(message);
    m_attachNoticeUntil = glfwGetTime() + 5.0;
}

PaneRenderOutcome ScopePaneRenderer::drawScopePanes(const PaneRenderInput& input)
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
    drawPaneContent(pass);
    ImGui::EndChild();

    return pass.outcome;
}

void ScopePaneRenderer::drawPaneContent(Pass& pass)
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
    const std::vector<std::string>& stack = m_view.stack();
    if (stack.size() == 1) {
        drawScopeById(stack.front(), pass);
    } else if (stack.size() > 1) {
        drawScopeStack(pass);
    }
}

void ScopePaneRenderer::drawScopeStack(Pass& pass)
{
    // Weights split the axis; a divider between each neighboring pair is a thin
    // grab strip that resizes them. Item spacing is zeroed so panes and dividers
    // tile the area exactly, and restored inside each pane so scope contents
    // keep their normal breathing room.
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const int count = static_cast<int>(m_view.stack().size());
    const float divider = DividerThickness * pass.input.uiScale;
    const std::vector<float> weights = m_view.stackWeights();
    const bool sideBySide = resolveSplitDirection(m_view.orientation(), area.x, area.y, weights, stackAspects(),
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
        drawScopeById(m_view.stack()[index], pass);
        ImGui::PopStyleVar();
        ImGui::EndChild();
        if (pane + 1 < count) {
            if (sideBySide) {
                ImGui::SameLine();
            }
            drawPaneDivider(pane, sideBySide, divider, area, lengths, pass);
            if (sideBySide) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::PopStyleVar();
}

std::vector<float> ScopePaneRenderer::stackAspects() const
{
    // A module's own declaration wins; the host table covers the color
    // picker and modules that declare nothing.
    std::vector<float> aspects;
    aspects.reserve(m_view.stack().size());
    for (const std::string& scopeId : m_view.stack()) {
        const HostScope* hostScope = m_registry.byId(scopeId);
        const float declared =
            hostScope != nullptr && hostScope->descriptor != nullptr ? hostScope->descriptor->preferred_aspect : 0.0f;
        aspects.push_back(declared > 0.0f ? declared : preferredScopeAspect(scopeId));
    }

    return aspects;
}

void ScopePaneRenderer::drawPaneDivider(int leftPane, bool sideBySide, float thickness, const ImVec2& area,
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

void ScopePaneRenderer::paintDivider(bool sideBySide, bool highlighted)
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

void ScopePaneRenderer::adjustDividerWeights(int leftPane, float deltaPixels, const std::vector<float>& lengths,
                                             float uiScale)
{
    if (deltaPixels == 0.0f) {
        return;
    }
    const auto left = static_cast<std::size_t>(leftPane);
    const std::string& leftId = m_view.stack()[left];
    const std::string& rightId = m_view.stack()[left + 1];
    const auto [newLeft, newRight] = dragDividerWeights(m_view.weight(leftId), m_view.weight(rightId), lengths[left],
                                                        lengths[left + 1], deltaPixels, MinPaneLength * uiScale);
    m_view.setWeight(leftId, newLeft);
    m_view.setWeight(rightId, newRight);
}

void ScopePaneRenderer::equalizeDividerWeights(int leftPane, Pass& pass)
{
    // Double-click reset: the two neighbors share their combined weight evenly,
    // leaving every other pane untouched.
    const auto left = static_cast<std::size_t>(leftPane);
    const std::string& leftId = m_view.stack()[left];
    const std::string& rightId = m_view.stack()[left + 1];
    const float average = (m_view.weight(leftId) + m_view.weight(rightId)) * 0.5f;
    m_view.setWeight(leftId, average);
    m_view.setWeight(rightId, average);
    pass.outcome.preferencesSaveDue = true;
    pass.outcome.activity = true;
}

void ScopePaneRenderer::drawScopeById(std::string_view id, Pass& pass)
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

void ScopePaneRenderer::drawVectorscopePane(Pass& pass)
{
    const DrawnScope scope = drawScopeImage(textureForId(VectorscopeScopeId), true, static_cast<float>(m_view.zoom()));
    const SsParamInfo* gain = firstParamOfKind(descriptorFor(VectorscopeScopeId), SS_PARAM_INTENSITY);
    if (gain != nullptr) {
        if (const auto adjusted = traceIntensityGesture(scope, VectorscopeScopeId, m_view.intensity(VectorscopeScopeId),
                                                        static_cast<float>(gain->default_value),
                                                        static_cast<float>(gain->intensity_shift), m_flash)) {
            m_view.setIntensity(VectorscopeScopeId, adjusted->intensity);
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

void ScopePaneRenderer::drawWaveformPane(std::string_view id, Pass& pass)
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

// Lazily rasterized textures for the embedded icon set, rebuilt when the
// framebuffer scale changes the requested pixel size.
ImTextureID ScopePaneRenderer::iconTextureId(Icon icon, int sizePixels)
{
    IconTexture& slot = m_iconTextures[static_cast<std::size_t>(icon)];
    if (!slot.texture || slot.sizePixels != sizePixels) {
        ScopeImage image;
        image.width = sizePixels;
        image.height = sizePixels;
        image.rgba = rasterizeIcon(icon, sizePixels);
        slot.texture = m_graphics.createScopeTexture(sizePixels, sizePixels);
        slot.texture->upload(image);
        slot.sizePixels = sizePixels;
    }

    return slot.texture->textureId();
}

std::string ScopePaneRenderer::bindingFor(std::string_view id) const
{
    return resolveBinding(m_scopeShortcuts, m_registry, id);
}

const SsScopeDescriptor* ScopePaneRenderer::descriptorFor(std::string_view id) const
{
    const HostScope* hostScope = m_registry.byId(id);

    return hostScope != nullptr ? hostScope->descriptor : nullptr;
}

const ScopeInstance* ScopePaneRenderer::projectionFor(std::string_view id) const
{
    const auto at = m_projections.find(std::string{id});

    return at != m_projections.end() ? &at->second : nullptr;
}

void ScopePaneRenderer::configureProjections()
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

HistogramStyle ScopePaneRenderer::histogramStyle() const
{
    const auto scope = m_analysis.scopeParams.find(HistogramScopeId);
    if (scope == m_analysis.scopeParams.end()) {
        return HistogramStyle::PerChannel;
    }
    const auto style = scope->second.find("style");
    const double value = style != scope->second.end() ? style->second : 0.0;

    return value < 0.5 ? HistogramStyle::PerChannel : HistogramStyle::Combined;
}

void ScopePaneRenderer::setWaveformGain(double gain)
{
    m_analysis.scopeParams[WaveformScopeId]["gain"] = gain;
    m_analysis.scopeParams[ParadeScopeId]["gain"] = gain;
}

const ScopeImage& ScopePaneRenderer::imageFor(std::string_view id) const
{
    static const ScopeImage empty;
    const auto at = m_output.images.find(std::string{id});

    return at != m_output.images.end() ? at->second : empty;
}

bool ScopePaneRenderer::hasTexture(std::string_view id) const
{
    return m_scopeTextures.find(std::string{id}) != m_scopeTextures.end();
}

ScopeTexture& ScopePaneRenderer::textureForId(std::string_view id)
{
    return *m_scopeTextures.at(std::string{id});
}

void ScopePaneRenderer::uploadScope(std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image)
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

void ScopePaneRenderer::uploadVisibleScopes()
{
    for (const std::string& id : m_view.stack()) {
        const auto texture = m_scopeTextures.find(id);
        if (texture == m_scopeTextures.end()) {
            continue;  // the color picker has no texture
        }
        uploadScope(texture->second, imageFor(id));
    }
}

ImVec2 ScopePaneRenderer::paneSizePoints(std::string_view id) const
{
    return m_panePoints[static_cast<std::size_t>(m_registry.indexOf(id))];
}

int ScopePaneRenderer::paneAt(const ImVec2& point) const
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
