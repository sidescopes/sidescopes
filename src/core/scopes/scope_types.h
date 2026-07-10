#pragma once

#include <cstdint>
#include <vector>

namespace sidescopes {

// RGB -> chroma matrix for the vectorscope. BT.601 is the convention most
// scope users know; BT.709 matches HD video material.
enum class ChromaMatrix { Bt601, Bt709 };

// What the waveform plots. RGB overlay is the default: separated colored
// traces make color casts readable at a glance. Parade shows the three
// channels side by side, each compressed to a third of the width.
enum class WaveformMode { Luma, Rgb, RgbAndLuma, RgbParade };

// A point in normalized scope-image coordinates, x and y in [0, 1].
struct NormalizedPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// CPU-side image a scope engine produces. The UI uploads `rgba` to a texture
// whenever `sequence` changes.
struct ScopeImage {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    uint64_t sequence = 0;
};

}  // namespace sidescopes
