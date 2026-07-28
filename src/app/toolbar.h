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
/// Nothing transient stands up here: every message the app has to give belongs
/// to the status bar, which keeps the foot of the window. The row drives the
/// picker directly when a region tool is clicked, and only what the host alone
/// can carry out travels back as a PaneRenderOutcome.
class Toolbar
{
public:
    /// @p icons is shared with the status bar, so a glyph both rows show is
    /// rasterized once. Every reference must outlive the toolbar.
    Toolbar(const ScopeRegistry& registry, ScopeView& view, const ShortcutResolver& shortcuts, RegionPicker& picker,
            IconTextures& icons);

    /// The scope selector: an icon button whose popup checklists every scope
    /// in the user's own order, which a row can be dragged to change and a
    /// toggle never disturbs. @p stackModifier is unused - the menu always
    /// toggles.
    [[nodiscard]] PaneRenderOutcome drawScopeToggles(bool stackModifier);

    /// The region toolbox: draw, attach to a window, attach to a face, and the
    /// clear, which stands down while there is no region to clear
    /// (@p regionSelected).
    [[nodiscard]] PaneRenderOutcome drawRegionToolIcons(bool regionSelected);

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

    /// Fills the open scope popup: every registered scope as a row, in the
    /// user's order. Applies any drag reorder to the view and records a toggle
    /// in @p outcome.
    void appendScopeMenu(PaneRenderOutcome& outcome);

    /// Draws scope @p id, the @p index'th row of the menu, as a checkbox, its
    /// name, and its key. Records a toggle in @p outcome.
    void drawScopeRow(const std::string& id, int index, const ScopeMenuColumns& cols, PaneRenderOutcome& outcome);

    /// Draws @p id's keyboard letter over the row just drawn, right-aligned a
    /// margin of @p rightPad in from the row's edge.
    void drawRowKey(std::string_view id, float rightPad) const;

    /// A scope's display name by id, or empty when the registry does not know
    /// it.
    [[nodiscard]] const char* scopeName(std::string_view id) const;

    const ScopeRegistry& m_registry;
    ScopeView& m_view;
    const ShortcutResolver& m_shortcuts;
    RegionPicker& m_picker;
    IconTextures& m_icons;
};

}  // namespace sidescopes
