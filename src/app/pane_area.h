#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_startup.h"
#include "app/pane_render.h"
#include "app/pin_board.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/scopes/histogram.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/graphics.h"

namespace sidescopes {

class CaptureController;

/// The collaborators the pane drawing reads and drives, bound once at startup.
/// Every one of them must outlive the pane area.
struct PaneAreaContext
{
    /// Rebuilds a scope texture whose image changed resolution.
    GraphicsBackend& graphics;
    /// What is on screen: the stack, the weights the dividers move, the
    /// graticule toggle, and each trace's intensity.
    ScopeView& view;
    const ScopeRegistry& registry;
    /// The worker's settings: the scope parameters the intensity gestures
    /// write, and the style the histogram pane draws itself in.
    AnalysisSettings& analysis;
    /// The worker's published images and the histogram's outline.
    const AnalysisWorker::Output& output;
    /// The capture state the help pages explain.
    const CaptureController& capture;
    /// The pinned reference colors the vectorscope and the picker draw.
    PinBoard& pins;
};

/// @brief The band between the toolbar and the status bar.
///
/// The capture help pages, or the scopes on screen stacked along the chosen
/// axis with a grab strip between each neighboring pair. It owns what only that
/// drawing needs: the scope textures, the projection instances the overlays are
/// built from, where each pane landed, and the trace intensity flash. It moves
/// the view's weights itself; only what the host alone can carry out travels
/// back as a PaneRenderOutcome.
class PaneArea
{
public:
    /// @p context binds the collaborators. @p projections are the host-side
    /// scope instances the overlays are drawn from and @p textures the blank
    /// scope textures and pane bookkeeping built at startup; the pane area
    /// takes ownership of both.
    PaneArea(const PaneAreaContext& context, std::map<std::string, ScopeInstance> projections,
             ScopeTextureSet textures);

    /// The pane area: the capture help pages, or the scopes on screen stacked
    /// along the chosen axis with a grab strip between each neighboring pair.
    [[nodiscard]] PaneRenderOutcome draw(const PaneRenderInput& input);

    /// Reconfigures every projection instance from the current settings,
    /// through the same parameter assembly the worker uses, so an overlay can
    /// never disagree with its trace.
    void configureProjections();

    /// Uploads the newest image into every on-screen scope's texture.
    void uploadVisibleScopes();

    /// @return Whether @p id draws a worker image at all; the host color picker
    ///         has no texture and asks the worker for nothing.
    [[nodiscard]] bool hasTexture(std::string_view id) const;

    /// @return The worker image behind @p id's texture, empty while there is
    ///         none yet.
    [[nodiscard]] const ScopeImage& imageFor(std::string_view id) const;

    /// @return The size @p id's pane last drew at, in interface points.
    [[nodiscard]] ImVec2 paneSizePoints(std::string_view id) const;

    /// @return The index of the pane @p point fell in, or -1 for none: which
    ///         pane a right-click landed in decides which options lead the
    ///         context menu.
    [[nodiscard]] int paneAt(const ImVec2& point) const;

private:
    /// One drawing pass: what the host supplied for this frame, and the
    /// outcome gathered from it as the panes draw, threaded through the steps
    /// so none of them keeps state of its own.
    struct Pass
    {
        const PaneRenderInput& input;
        PaneRenderOutcome outcome;
    };

    [[nodiscard]] const SsScopeDescriptor* descriptorFor(std::string_view id) const;
    [[nodiscard]] const ScopeInstance* projectionFor(std::string_view id) const;
    [[nodiscard]] HistogramStyle histogramStyle() const;
    void setWaveformGain(double gain);
    [[nodiscard]] ScopeTexture& textureForId(std::string_view id);
    void uploadScope(std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image);
    /// What fills the pane area: the capture help pages, or the scope stack.
    void drawContent(Pass& pass);
    void drawStack(Pass& pass);
    /// Each stacked scope's preferred pane aspect, in stack order, for the
    /// Automatic split scoring.
    [[nodiscard]] std::vector<float> stackAspects() const;
    void drawDivider(int leftPane, bool sideBySide, float thickness, const ImVec2& area,
                     const std::vector<float>& lengths, Pass& pass);
    static void paintDivider(bool sideBySide, bool highlighted);
    void adjustDividerWeights(int leftPane, float deltaPixels, const std::vector<float>& lengths, float uiScale);
    void equalizeDividerWeights(int leftPane, Pass& pass);
    void drawScopeById(std::string_view id, Pass& pass);
    void drawVectorscopePane(Pass& pass);
    void drawWaveformPane(std::string_view id, Pass& pass);

    GraphicsBackend& m_graphics;
    ScopeView& m_view;
    const ScopeRegistry& m_registry;
    AnalysisSettings& m_analysis;
    const AnalysisWorker::Output& m_output;
    const CaptureController& m_capture;
    PinBoard& m_pins;

    std::map<std::string, ScopeInstance> m_projections;
    std::map<std::string, std::unique_ptr<ScopeTexture>> m_scopeTextures;

    // Where the panes landed last frame, at each scope's own identity index:
    // the size the adaptive detail follows, the rectangle the context menu
    // reads back, and the child ids the panes and dividers are drawn under.
    std::vector<ImVec2> m_panePoints;
    std::vector<ImVec4> m_paneRects;
    std::vector<std::string> m_paneIds;
    std::vector<std::string> m_dividerIds;
    std::vector<ImVec2> m_histogramScratch;

    TraceFlash m_flash;
};

}  // namespace sidescopes
