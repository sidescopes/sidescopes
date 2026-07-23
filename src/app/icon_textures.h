#pragma once

#include <array>
#include <memory>

#include "imgui.h"
#include "platform/graphics.h"
#include "platform/icons.h"

namespace sidescopes {

/// The pixel size the icon textures rasterize at: the toolbar square in
/// framebuffer pixels, so the strokes land on the physical grid.
[[nodiscard]] int iconPixelSize();

/// @brief The embedded icon set as textures, rasterized on demand.
///
/// Both rows that carry icons - the toolbar above the panes and the status bar
/// below them - draw from one cache, so a glyph is rasterized once per size
/// however many rows show it.
class IconTextures
{
public:
    explicit IconTextures(GraphicsBackend& graphics);

    /// @return The texture for @p icon at @p sizePixels, rasterized on first
    ///         ask and rebuilt when the display changes the size asked for.
    [[nodiscard]] ImTextureID textureId(Icon icon, int sizePixels);

private:
    /// A lazily rasterized texture for one of the embedded set's icons,
    /// rebuilt when the requested pixel size changes with the display.
    struct Slot
    {
        std::unique_ptr<ScopeTexture> texture;
        int sizePixels = 0;
    };

    GraphicsBackend& m_graphics;
    std::array<Slot, IconCount> m_slots;
};

}  // namespace sidescopes
