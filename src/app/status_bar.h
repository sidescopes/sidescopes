#pragma once

#include <optional>
#include <string>

#include "app/icon_textures.h"
#include "app/shortcut_resolver.h"
#include "core/frame.h"

namespace sidescopes {

class RegionPicker;

/// The status bar's reserved height below the panes: the spacing that parts the
/// strip from the panes, the tallest thing that can stand on its row, and the
/// offset that centres the row between them. The pane area reserves it, so the
/// bar keeps the foot of the window in every state.
[[nodiscard]] float statusBarHeight();

/// @brief The reserved strip under the panes.
///
/// The pin tool holds the left corner and the live swatch the right, with the
/// channel readout gathered against it; a message clears the row and takes it
/// whole. It owns that message and how long it stands.
class StatusBar
{
public:
    /// @p icons is shared with the toolbar, so a glyph both rows show is
    /// rasterized once. Every reference must outlive the bar.
    StatusBar(const ShortcutResolver& shortcuts, RegionPicker& picker, IconTextures& icons);

    /// Draws the strip: the pin tool, which stands down without a scope that
    /// takes pins (@p pinsAvailable), and the readout for @p cursorColor.
    void draw(bool pinsAvailable, const std::optional<FloatColor>& cursorColor);

    /// Shows @p message in the status strip for the next couple of seconds.
    void setStatus(std::string message);

private:
    /// The bar's colour sampler, anchored to the strip's left corner.
    void drawPinTool(bool pinsAvailable);

    const ShortcutResolver& m_shortcuts;
    RegionPicker& m_picker;
    IconTextures& m_icons;

    std::string m_message;
    double m_until = 0.0;
};

}  // namespace sidescopes
