#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

inline constexpr int kDefaultVectorscopeSize = 256;

struct VectorscopeSettings {
    // Trace gain applied to normalized bin densities before log mapping.
    float gain = 3.0f;
    // Sample every Nth pixel horizontally and vertically (1..8).
    int sampling_stride = 1;
    ChromaMatrix matrix = ChromaMatrix::Bt601;
    // Display image resolution per axis. Accumulation always happens on
    // the 256-code chroma grid - 8-bit content quantizes to it, and a
    // finer accumulation grid renders the quantization as gridded
    // texture. A finer IMAGE is interpolated from the code grid, which
    // keeps big panes smooth while the fractional splat preserves
    // sub-code peak positions.
    int size = kDefaultVectorscopeSize;
};

// Classic vectorscope: a 256x256 chroma density plot. Bins are accumulated
// on integer chroma coordinates and mapped to a display image with a
// logarithmic curve normalized to the densest bin, so sparse traces stay
// visible while dominant content clearly dominates. Densities are
// per-sample, which keeps the gain setting stable across sampling strides
// and region sizes.
//
// Not thread-safe; a single analysis thread owns each instance.
class Vectorscope {
public:
    static constexpr int kSize = kDefaultVectorscopeSize;

    Vectorscope();

    void Configure(const VectorscopeSettings& settings);
    void Accumulate(const FrameView& frame, IntRect region);
    [[nodiscard]] const ScopeImage& Image() const { return image_; }

    // Where a color lands on the scope, in normalized image coordinates.
    // Graticule targets, the cursor marker, and any future indicators all go
    // through this projection, which guarantees overlays agree with the data.
    [[nodiscard]] std::optional<NormalizedPoint> Project(const FloatColor& color) const;

private:
    void Resize(int size);
    void RebuildTintTable();
    void MapBinsToImage(uint64_t sample_count);

    VectorscopeSettings settings_;
    int size_ = kDefaultVectorscopeSize;
    std::vector<uint32_t> bins_;    // always kSize x kSize (code grid)
    std::vector<float> smoothed_;   // code grid, post-kernel densities
    std::vector<float> upsampled_;  // size_ x size_ when finer than kSize
    std::vector<uint8_t> tint_;
    ScopeImage image_;
};

}  // namespace sidescopes
