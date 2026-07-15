#pragma once

#include <cstdint>
#include <vector>

namespace sidescopes {

// RGB -> chroma matrix for the vectorscope. BT.709 is what today's scope
// users expect (every HD-era tool measures with it); BT.601 remains for
// reading SD material and codec-native JPEG chroma.
enum class ChromaMatrix
{
    Bt601,
    Bt709
};

// How trace density maps to brightness. Boosted lifts dim regions with a
// log curve so sparse traces stay readable; Linear behaves like a
// phosphor scope, where only genuinely dense mass glows brightly and
// faint spread stays faint.
enum class TraceResponse
{
    Boosted,
    Linear
};

// What the waveform plots. RGB overlay is the default: separated colored
// traces make color casts readable at a glance. ColoredLuma plots
// luminance and paints each part of the trace in the average color of
// the pixels that put it there. Parade shows the three channels side by
// side, each compressed to a third of the width.
enum class WaveformMode
{
    Luma,
    Rgb,
    RgbAndLuma,
    RgbParade,
    ColoredLuma
};

// A point in normalized scope-image coordinates, x and y in [0, 1].
struct NormalizedPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

// CPU-side image a scope engine produces. The UI uploads `rgba` to a texture
// whenever `sequence` changes.
struct ScopeImage
{
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    uint64_t sequence = 0;
};

}  // namespace sidescopes
