#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/app_startup.h"
#include "app/pin_board.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "core/preferences.h"
#include "core/scopes/histogram.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/graphics.h"
#include "platform/icons.h"

namespace sidescopes {

class CaptureController;
class RegionPicker;

/// The collaborators the pane rendering reads and drives, bound once at
/// startup. Every one of them must outlive the renderer.
struct ScopePaneContext
{
    /// Rasterizes the icon glyphs, and rebuilds a scope texture whose image
    /// changed resolution.
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
    /// The region tools request their picks straight from the picker.
    RegionPicker& regionPicker;
    /// The pinned reference colors the vectorscope and the picker draw.
    PinBoard& pins;
    /// The bindings the tool tooltips name their keys by.
    const ShortcutBindings& shortcuts;
    /// The per-scope shortcut overrides the chip tooltips resolve through.
    const std::map<std::string, std::string>& scopeShortcuts;
};

/// What one frame of pane drawing reads from the host: the values it
/// recomputes every frame, rather than the state the renderer owns or the
/// collaborators it holds. The references stay valid for the single
/// synchronous draw call.
struct PaneRenderInput
{
    /// The interface scale the divider thickness and the smallest pane are
    /// measured in.
    float uiScale;
    /// Whether the region the scopes read already covers the display, which
    /// the reset tool shows as already done.
    bool regionIsFullScreen;
    /// Whether a scope that takes pins is on screen; without one the pin tool
    /// stands down.
    bool pinsAvailable;
    /// The smoothed color under the cursor, per trace, empty until a sample
    /// lands.
    const std::optional<FloatColor>& vectorscopeColor;
    const std::optional<FloatColor>& waveformColor;
    /// The fixed-width companion font the picker aligns hex codes with; null
    /// when the system had none.
    ImFont* monospaceFont;
};

/// A scope a toolbar chip chose, and whether it joins the scopes on screen or
/// replaces them.
struct ScopeChoice
{
    std::string id;
    bool stack = false;
};

/// What one pane-drawing pass decided that the host must apply. The renderer
/// drives the collaborators it holds - the view's weights, the picker's
/// requests, the pin board - itself; only what the host alone can carry out
/// travels here. Several fields can be set at once, so each is applied in turn.
struct PaneRenderOutcome
{
    /// A chip chose a scope: the host shows it, which may wait briefly for the
    /// worker to fill its image.
    std::optional<ScopeChoice> chosenScope;
    /// The reset tool: the host drops every region and attachment.
    bool resetToFullScreen = false;
    /// A scope parameter changed: the host pushes the settings to the worker.
    bool analysisDirty = false;
    /// Interaction happened worth marking: the host stamps its activity clock.
    bool activity = false;
    /// A persisted value changed: the host schedules a preferences save.
    bool preferencesSaveDue = false;
};

/// Draws the scopes' side of the main window - the scope chips, the region
/// toolbox, the pane area with its dividers, and the status strip - and owns
/// what only that drawing needs: the scope and icon textures, the projection
/// instances the overlays are built from, where each pane landed, the trace
/// intensity flash, and the two transient lines the toolbar and the strip
/// show. It reads and drives the collaborators it is constructed with, so the
/// host actions a click resolves to - showing a scope, resetting to full
/// screen, the clocks the whole shell shares - are all that travel back, as a
/// PaneRenderOutcome the host applies.
class ScopePaneRenderer
{
public:
    /// @p context binds the collaborators. @p projections are the host-side
    /// scope instances the overlays are drawn from and @p textures the blank
    /// scope textures and pane bookkeeping built at startup; the renderer takes
    /// ownership of both.
    ScopePaneRenderer(const ScopePaneContext& context, std::map<std::string, ScopeInstance> projections,
                      ScopeTextureSet textures);

    /// The scope letter chips, one per scope the registry lettered. Switching
    /// is the common case, so a plain click shows one scope alone and
    /// @p stackModifier stacks.
    [[nodiscard]] PaneRenderOutcome drawScopeToggles(bool stackModifier);

    /// The region toolbox: draw, attach to a window, attach to a face, and the
    /// reset to full screen.
    [[nodiscard]] PaneRenderOutcome drawRegionToolIcons(const PaneRenderInput& input);

    /// The pane area: the capture help pages, or the scopes on screen stacked
    /// along the chosen axis with a grab strip between each neighboring pair.
    [[nodiscard]] PaneRenderOutcome drawScopePanes(const PaneRenderInput& input);

    /// The reserved strip under the panes: the pin tool in the left corner,
    /// the live swatch in the right, messages and the readout between them.
    void drawStatusBar(const PaneRenderInput& input);

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

    /// Shows @p message in the status strip for the next couple of seconds.
    void setStatus(std::string message);

    /// Shows @p message beside the scope chips for the next few seconds: the
    /// note an attached window leaves when it closes out from under its region.
    void showAttachNotice(std::string message);

private:
    /// A lazily rasterized texture for one of the embedded set's icons,
    /// rebuilt when the requested pixel size changes with the display.
    struct IconTexture
    {
        std::unique_ptr<ScopeTexture> texture;
        int sizePixels = 0;
    };

    /// One drawing pass: what the host supplied for this frame, and the
    /// outcome gathered from it as the panes draw, threaded through the steps
    /// so none of them keeps state of its own.
    struct Pass
    {
        const PaneRenderInput& input;
        PaneRenderOutcome outcome;
    };

    [[nodiscard]] ImTextureID iconTextureId(Icon icon, int sizePixels);
    [[nodiscard]] std::string bindingFor(std::string_view id) const;
    [[nodiscard]] const SsScopeDescriptor* descriptorFor(std::string_view id) const;
    [[nodiscard]] const ScopeInstance* projectionFor(std::string_view id) const;
    [[nodiscard]] HistogramStyle histogramStyle() const;
    void setWaveformGain(double gain);
    [[nodiscard]] ScopeTexture& textureForId(std::string_view id);
    void uploadScope(std::unique_ptr<ScopeTexture>& texture, const ScopeImage& image);
    /// Seats the constant-width region toolbox: right-aligned beside the
    /// scopes, flush left on its own wrapped row, attach notice on the left.
    void placeRegionToolbox();
    /// The bar's colour sampler, anchored to the strip's left corner.
    void drawPinTool(bool pinsAvailable);
    /// Draws the swatch inwards from the right corner, preceded by the
    /// channel readout when the room left by \p taken - the row's used width
    /// so far - allows it.
    static void drawCursorReadout(float taken, const std::optional<FloatColor>& color);
    /// What fills the pane area: the capture help pages, or the scope stack.
    void drawPaneContent(Pass& pass);
    void drawScopeStack(Pass& pass);
    /// Each stacked scope's preferred pane aspect, in stack order, for the
    /// Automatic split scoring.
    [[nodiscard]] std::vector<float> stackAspects() const;
    void drawPaneDivider(int leftPane, bool sideBySide, float thickness, const ImVec2& area,
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
    RegionPicker& m_regionPicker;
    PinBoard& m_pins;
    const ShortcutBindings& m_shortcuts;
    const std::map<std::string, std::string>& m_scopeShortcuts;

    std::map<std::string, ScopeInstance> m_projections;
    std::map<std::string, std::unique_ptr<ScopeTexture>> m_scopeTextures;
    std::array<IconTexture, IconCount> m_iconTextures;

    // Where the panes landed last frame, at each scope's own identity index:
    // the size the adaptive detail follows, the rectangle the context menu
    // reads back, and the child ids the panes and dividers are drawn under.
    std::vector<ImVec2> m_panePoints;
    std::vector<ImVec4> m_paneRects;
    std::vector<std::string> m_paneIds;
    std::vector<std::string> m_dividerIds;
    std::vector<ImVec2> m_histogramScratch;

    TraceFlash m_flash;
    // The two transient lines: a status message takes the strip under the
    // panes, the attach notice sits beside the scope chips.
    std::string m_statusMessage;
    double m_statusUntil = 0.0;
    std::string m_attachNotice;
    double m_attachNoticeUntil = 0.0;
};

}  // namespace sidescopes
