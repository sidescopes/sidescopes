#pragma once

#include <map>
#include <string>
#include <string_view>

#include "app/app_startup.h"
#include "app/icon_textures.h"
#include "app/pane_area.h"
#include "app/pane_render.h"
#include "app/pin_board.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"
#include "app/status_bar.h"
#include "app/toolbar.h"
#include "core/analysis_worker.h"
#include "core/frame.h"
#include "imgui.h"
#include "modules/module_registry.h"
#include "platform/graphics.h"

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
    /// The bindings the tool and chip tooltips name their keys by.
    const ShortcutResolver& shortcuts;
};

/// @brief The scopes' side of the main window, in three bands.
///
/// The toolbar of scope chips and region tools, the pane area with its
/// dividers, and the status strip. Each band owns its own state and is drawn on
/// its own, because the host works between them - it resolves shortcuts after
/// the chips and opens the context menu after the strip - so it owns the three
/// rather than folding them into one pass, plus the icon textures two of them
/// share. Host actions a click resolves to travel back as a PaneRenderOutcome
/// the host applies.
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
    // The icon cache is declared first: both rows that draw glyphs hold a
    // reference to it.
    IconTextures m_icons;
    Toolbar m_toolbar;
    PaneArea m_panes;
    StatusBar m_statusBar;
};

}  // namespace sidescopes
