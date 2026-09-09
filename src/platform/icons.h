#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/region_kind.h"

namespace sidescopes {

/// The application's icon set. One set of embedded vector sources drawn
/// identically on every platform - toolbar and border chrome alike.
enum class Icon
{
    Pin,
    PinOff,
    SquarePen,
    Pencil,
    User,
    Pipette,
    SquareOff,
    ChartColumn,
    PenLine,
    PanelsTopLeft,
    Save,
};

/// The number of icons in the set, for texture caches sized by Icon.
constexpr std::size_t IconCount = 11;

/// Rasterizes @p icon into a tightly packed RGBA8 square of
/// @p sizePixels a side: near-white strokes on transparency, ready to
/// upload as a texture or wrap in a platform image. Returns an empty
/// vector only if the embedded source fails to parse, which a unit test
/// rules out.
[[nodiscard]] std::vector<uint8_t> rasterizeIcon(Icon icon, int sizePixels);

/// The border control communicates whether the region is attached to a window
/// or global, using the pin and pin-off glyphs.
[[nodiscard]] Icon iconForRegionKind(RegionKind kind);

}  // namespace sidescopes
