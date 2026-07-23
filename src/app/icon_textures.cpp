#include "app/icon_textures.h"

#include <cmath>
#include <cstddef>

#include "core/frame.h"
#include "imgui.h"

namespace sidescopes {

int iconPixelSize()
{
    return static_cast<int>(std::lround(ImGui::GetTextLineHeight() * ImGui::GetIO().DisplayFramebufferScale.x));
}

IconTextures::IconTextures(GraphicsBackend& graphics)
    : m_graphics(graphics)
{
}

// Lazily rasterized textures for the embedded icon set, rebuilt when the
// framebuffer scale changes the requested pixel size.
ImTextureID IconTextures::textureId(Icon icon, int sizePixels)
{
    Slot& slot = m_slots[static_cast<std::size_t>(icon)];
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

}  // namespace sidescopes
