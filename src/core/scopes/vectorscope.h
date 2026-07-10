#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

struct VectorscopeSettings {
    // Trace gain applied to normalized bin densities before log mapping.
    float gain = 3.0f;
    // Sample every Nth pixel horizontally and vertically (1..8).
    int sampling_stride = 1;
    ChromaMatrix matrix = ChromaMatrix::Bt601;
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
    static constexpr int kSize = 256;

    Vectorscope();

    void Configure(const VectorscopeSettings& settings);
    void Accumulate(const FrameView& frame, IntRect region);
    [[nodiscard]] const ScopeImage& Image() const { return image_; }

    // Where a color lands on the scope, in normalized image coordinates.
    // Graticule targets, the cursor marker, and any future indicators all go
    // through this projection, which guarantees overlays agree with the data.
    [[nodiscard]] std::optional<NormalizedPoint> Project(const FloatColor& color) const;

private:
    void RebuildTintTable();
    void MapBinsToImage(uint64_t sample_count);

    VectorscopeSettings settings_;
    std::vector<uint32_t> bins_;
    std::array<uint8_t, kSize * kSize * 3> tint_{};
    ScopeImage image_;
};

}  // namespace sidescopes
