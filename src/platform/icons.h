#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sidescopes {

/// The application's icon set. One set of embedded vector sources drawn
/// identically on every platform - toolbar and border chrome alike.
enum class Icon
{
    Pin,
    PinOff,
    Paperclip,
    SquarePen,
    User,
    Pipette,
    Expand,
};

/// The number of icons in the set, for texture caches sized by Icon.
constexpr std::size_t IconCount = 7;

/// Rasterizes @p icon into a tightly packed RGBA8 square of
/// @p sizePixels a side: near-white strokes on transparency, ready to
/// upload as a texture or wrap in a platform image. Returns an empty
/// vector only if the embedded source fails to parse, which a unit test
/// rules out.
[[nodiscard]] std::vector<uint8_t> rasterizeIcon(Icon icon, int sizePixels);

}  // namespace sidescopes
