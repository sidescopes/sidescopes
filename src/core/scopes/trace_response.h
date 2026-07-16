#pragma once

#include <cmath>

namespace sidescopes {

/// The mid-density gamma the vectorscope and the waveform both apply after
/// the log curve: normalizing to the densest bin pushes the mid-body of the
/// trace down, and this lift keeps it readable at any gain.
inline constexpr float MidDensityGamma = 0.65f;

/// Applies the mid-density gamma to a log-normalized density in [0, 1], in
/// the caller's own precision.
template <typename Float>
[[nodiscard]] inline Float applyMidDensityGamma(Float normalized)
{
    return std::pow(normalized, static_cast<Float>(MidDensityGamma));
}

/// The four Catmull-Rom weights for interpolation parameter @p t in [0, 1],
/// applied as w0*p[-1] + w1*p[0] + w2*p[1] + w3*p[2]. Shared by the waveform
/// level-axis spline and the histogram curve; each caller keeps its own
/// numeric precision.
template <typename Float>
struct CatmullRomWeights
{
    Float w0;
    Float w1;
    Float w2;
    Float w3;
};

template <typename Float>
[[nodiscard]] inline CatmullRomWeights<Float> catmullRomWeights(Float t)
{
    const Float t2 = t * t;
    const Float t3 = t2 * t;
    return CatmullRomWeights<Float>{
        static_cast<Float>(-0.5) * t3 + t2 - static_cast<Float>(0.5) * t,
        static_cast<Float>(1.5) * t3 - static_cast<Float>(2.5) * t2 + static_cast<Float>(1.0),
        static_cast<Float>(-1.5) * t3 + static_cast<Float>(2.0) * t2 + static_cast<Float>(0.5) * t,
        static_cast<Float>(0.5) * t3 - static_cast<Float>(0.5) * t2,
    };
}

}  // namespace sidescopes
