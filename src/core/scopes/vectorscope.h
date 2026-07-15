#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/frame.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

inline constexpr int DefaultVectorscopeSize = 256;

struct VectorscopeSettings
{
    // Trace gain applied to normalized bin densities before log mapping.
    float gain = 3.0f;
    // Sample every Nth pixel horizontally and vertically (1..8).
    int samplingStride = 1;
    ChromaMatrix matrix = ChromaMatrix::Bt709;
    TraceResponse response = TraceResponse::Boosted;
    // Display image resolution per axis. Accumulation always happens on
    // the 256-code chroma grid - 8-bit content quantizes to it, and a
    // finer accumulation grid renders the quantization as gridded
    // texture. A finer IMAGE is interpolated from the code grid, which
    // keeps big panes smooth while the fractional splat preserves
    // sub-code peak positions.
    int size = DefaultVectorscopeSize;
};

// Classic vectorscope: a 256x256 chroma density plot. Bins are accumulated
// on integer chroma coordinates and mapped to a display image with a
// logarithmic curve normalized to the densest bin, so sparse traces stay
// visible while dominant content clearly dominates. Densities are
// per-sample, which keeps the gain setting stable across sampling strides
// and region sizes.
//
// Not thread-safe; a single analysis thread owns each instance.
class Vectorscope
{
public:
    static constexpr int Size = DefaultVectorscopeSize;

    Vectorscope();

    void configure(const VectorscopeSettings& settings);
    void accumulate(const FrameView& frame, IntRect region);

    [[nodiscard]] const ScopeImage& image() const
    {
        return m_image;
    }

    // Where a color lands on the scope, in normalized image coordinates.
    // Graticule targets, the cursor marker, and any future indicators all go
    // through this projection, which guarantees overlays agree with the data.
    [[nodiscard]] std::optional<NormalizedPoint> project(const FloatColor& color) const;

private:
    void resize(int size);
    void rebuildTintTable();
    void mapBinsToImage(uint64_t sampleCount);

    VectorscopeSettings m_settings;
    int m_size = DefaultVectorscopeSize;
    std::vector<uint32_t> m_bins;    // always Size x Size (code grid)
    std::vector<float> m_smoothed;   // code grid, post-kernel densities
    std::vector<float> m_upsampled;  // size_ x size_ when finer than Size
    std::vector<uint8_t> m_tint;
    ScopeImage m_image;
};

}  // namespace sidescopes
