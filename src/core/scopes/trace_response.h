#pragma once

#include <cmath>

namespace sidescopes {

/// The mid-density gamma the waveform applies after the log curve, and the
/// vectorscope's own default: normalizing to the densest bin pushes the
/// mid-body of the trace down, and this lift keeps it readable at any gain.
/// The waveform holds it fixed; the vectorscope offers it as a setting and
/// starts here, so the two scopes agree until the setting is moved.
inline constexpr float MidDensityGamma = 0.65f;

/// Applies a mid-density @p gamma to a log-normalized density in [0, 1], in
/// the caller's own precision.
template <typename Float>
[[nodiscard]] inline Float applyTraceGamma(Float normalized, Float gamma)
{
    return std::pow(normalized, gamma);
}

/// Applies the fixed mid-density gamma, for a scope that does not offer it.
template <typename Float>
[[nodiscard]] inline Float applyMidDensityGamma(Float normalized)
{
    return applyTraceGamma(normalized, static_cast<Float>(MidDensityGamma));
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
