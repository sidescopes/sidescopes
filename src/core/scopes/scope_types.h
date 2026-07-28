#pragma once

#include <cstdint>
#include <vector>

namespace sidescopes {

/// What the waveform plots. RGB overlay is the default: separated colored
/// traces make color casts readable at a glance. ColoredLuma plots
/// luminance and paints each part of the trace in the average color of
/// the pixels that put it there. Parade shows the three channels side by
/// side, each compressed to a third of the width.
enum class WaveformMode
{
    Luma,
    Rgb,
    RgbAndLuma,
    RgbParade,
    ColoredLuma
};

/// A point in normalized scope-image coordinates, x and y in [0, 1].
struct NormalizedPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

/// CPU-side image a scope engine produces. The UI uploads @c rgba to a texture
/// whenever @c sequence changes.
struct ScopeImage
{
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    uint64_t sequence = 0;
};

}  // namespace sidescopes
