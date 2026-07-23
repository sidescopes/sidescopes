#pragma once

#include <string>

#include "app/icon_textures.h"
#include "app/pane_render.h"
#include "app/scope_registry.h"
#include "app/scope_view.h"
#include "app/shortcut_resolver.h"

namespace sidescopes {

class RegionPicker;

/// @brief The row above the panes: the scope chips and the region toolbox.
///
/// It owns the one line that comes and goes up here - the note an attached
/// window leaves when it closes out from under its region - and drives the
/// picker directly when a region tool is clicked. Only what the host alone can
/// carry out travels back as a PaneRenderOutcome.
class Toolbar
{
public:
    /// @p icons is shared with the status bar, so a glyph both rows show is
    /// rasterized once. Every reference must outlive the toolbar.
    Toolbar(const ScopeRegistry& registry, ScopeView& view, const ShortcutResolver& shortcuts, RegionPicker& picker,
            IconTextures& icons);

    /// The scope letter chips, one per scope the registry lettered. Switching
    /// is the common case, so a plain click shows one scope alone and
    /// @p stackModifier stacks.
    [[nodiscard]] PaneRenderOutcome drawScopeToggles(bool stackModifier);

    /// The region toolbox: draw, attach to a window, attach to a face, and the
    /// reset to full screen, which stands down while the region already covers
    /// the display (@p regionIsFullScreen).
    [[nodiscard]] PaneRenderOutcome drawRegionToolIcons(bool regionIsFullScreen);

    /// Shows @p message beside the scope chips for the next few seconds: the
    /// note an attached window leaves when it closes out from under its region.
    void showAttachNotice(std::string message);

private:
    /// Seats the constant-width region toolbox: right-aligned beside the
    /// scopes, flush left on its own wrapped row, attach notice on the left.
    void placeRegionToolbox() const;

    const ScopeRegistry& m_registry;
    ScopeView& m_view;
    const ShortcutResolver& m_shortcuts;
    RegionPicker& m_picker;
    IconTextures& m_icons;

    std::string m_attachNotice;
    double m_attachNoticeUntil = 0.0;
};

}  // namespace sidescopes
