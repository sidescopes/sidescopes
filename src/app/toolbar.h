#pragma once

#include <string>
#include <string_view>
#include <vector>

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

    /// The scope selector: an icon button whose popup checklists every scope,
    /// the ones on screen leading in pane order and draggable to rearrange
    /// them. @p stackModifier is unused - the menu always toggles.
    [[nodiscard]] PaneRenderOutcome drawScopeToggles(bool stackModifier);

    /// The region toolbox: draw, attach to a window, attach to a face, and the
    /// clear, which stands down while there is no region to clear
    /// (@p regionSelected).
    [[nodiscard]] PaneRenderOutcome drawRegionToolIcons(bool regionSelected);

    /// Shows @p message beside the scope chips for the next few seconds: the
    /// note an attached window leaves when it closes out from under its region.
    void showAttachNotice(std::string message);

private:
    /// The column geometry the scope menu lays its rows on: where the name
    /// starts, the total row width, and the margin the keys keep from the right
    /// edge - so names left-align and the keys right-align clear of the border.
    struct ScopeMenuColumns
    {
        float nameX;
        float width;
        float rightPad;
    };

    /// Measures the menu's columns from the widest scope name and key.
    [[nodiscard]] ScopeMenuColumns scopeColumns() const;

    /// Fills the open scope popup: the scopes on screen first, then a rule and
    /// the rest. Records any drag reorder and toggle in @p outcome.
    void appendScopeMenu(PaneRenderOutcome& outcome);

    /// Draws the on-screen scopes as draggable rows and lands any reorder.
    /// @return Whether a drop changed the order.
    bool appendShownScopes(std::vector<std::string>& shown, const ScopeMenuColumns& cols, PaneRenderOutcome& outcome);

    /// Draws one not-yet-shown scope as a row that adds it when clicked.
    /// @return Whether the row was clicked.
    bool drawAddRow(const std::string& id, const ScopeMenuColumns& cols);

    /// Draws @p id's keyboard letter over the row just drawn, right-aligned a
    /// margin of @p rightPad in from the row's edge.
    void drawRowKey(std::string_view id, float rightPad) const;

    /// A scope's display name by id, or empty when the registry does not know
    /// it.
    [[nodiscard]] const char* scopeName(std::string_view id) const;

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
