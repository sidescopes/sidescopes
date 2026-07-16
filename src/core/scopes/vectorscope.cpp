#include "core/scopes/vectorscope.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/scopes/trace_response.h"

namespace sidescopes {
namespace {

// Fixed-point (x256) full-range RGB -> Cb/Cr coefficients from the BT.601
// and BT.709 specifications. Chroma lands in [0, 255] with neutral at 128.
struct ChromaCoefficients
{
    int cbFromR, cbFromG, cbFromB;
    int crFromR, crFromG, crFromB;
};

// Each row sums to zero exactly: a gray input must land dead on the
// neutral center, so the rounding error goes into the green coefficient
// (0.3% of its weight) rather than into a bias of the neutral axis.
constexpr ChromaCoefficients Bt601{-38, -74, 112, 112, -94, -18};
constexpr ChromaCoefficients Bt709{-26, -86, 112, 112, -102, -10};

constexpr const ChromaCoefficients& coefficientsFor(ChromaMatrix matrix)
{
    return matrix == ChromaMatrix::Bt601 ? Bt601 : Bt709;
}

// Densities are normalized to a nominal sample count so the same scene maps
// to the same trace regardless of sampling stride or region size.
constexpr double ReferenceSampleCount = 1'000'000.0;

// A half-strength 3x3 binomial on a square grid: half the source value plus
// a half-weighted, normalized 1-2-1 x 1-2-1 blur. Shared by the code-grid
// smoothing and the display-image settling pass, which differ only in their
// source type and grid size. The full-strength variant in the adaptive
// estimator stays separate.
template <typename Src>
void halfBinomial(const Src* in, int size, float* out)
{
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            const auto at = [&](int y, int x) -> float {
                if (y < 0 || y >= size || x < 0 || x >= size) {
                    return 0.0f;
                }
                return static_cast<float>(in[static_cast<std::size_t>(y) * size + x]);
            };
            const auto row = [&](int y) { return at(y, px - 1) + 2.0f * at(y, px) + at(y, px + 1); };
            out[static_cast<std::size_t>(py) * size + px] =
                0.5f * at(py, px) + 0.5f * (row(py - 1) + 2.0f * row(py) + row(py + 1)) / 16.0f;
        }
    }
}

}  // namespace

Vectorscope::Vectorscope()
{
    resize(DefaultVectorscopeSize);
}

void Vectorscope::configure(const VectorscopeSettings& settings)
{
    const bool matrixChanged = settings.matrix != m_settings.matrix;
    m_settings = settings;
    m_settings.samplingStride = std::clamp(m_settings.samplingStride, 1, 8);
    m_settings.size = std::clamp(m_settings.size, CodeGridSize, 512);
    if (m_settings.size != m_imageSize) {
        resize(m_settings.size);
    } else if (matrixChanged) {
        rebuildTintTable();
    }
}

void Vectorscope::resize(int size)
{
    m_imageSize = size;
    m_bins.assign(static_cast<std::size_t>(CodeGridSize) * CodeGridSize, 0);
    m_smoothed.assign(static_cast<std::size_t>(CodeGridSize) * CodeGridSize, 0.0f);
    m_upsampled.assign(m_imageSize > CodeGridSize ? static_cast<std::size_t>(m_imageSize) * m_imageSize : 0, 0.0f);
    m_tint.assign(static_cast<std::size_t>(m_imageSize) * m_imageSize * 3, 0);
    m_image.width = m_imageSize;
    m_image.height = m_imageSize;
    m_image.rgba.assign(static_cast<std::size_t>(m_imageSize) * m_imageSize * 4, 0);
    rebuildTintTable();
}

void Vectorscope::accumulate(const FrameView& frame, IntRect region)
{
    region = region.clampedTo(frame.width, frame.height);
    std::fill(m_bins.begin(), m_bins.end(), 0u);

    uint64_t sampleCount = 0;
    if (!region.empty()) {
        const ChromaCoefficients& matrix = coefficientsFor(m_settings.matrix);
        const int stride = m_settings.samplingStride;
        for (int py = region.y; py < region.y + region.height; py += stride) {
            const uint8_t* pixel = frame.pixelAt(region.x, py);
            const uint8_t* rowEnd = frame.pixelAt(region.x + region.width, py);
            // Accumulation always uses the 256-code grid: 8-bit content
            // quantizes its chroma to those codes (and piles unevenly
            // onto fixed sub-code positions, measured at 45% of a grass
            // frame on a single one), so a finer accumulation grid
            // renders quantization as gridded texture. Samples splat
            // bilinearly across the four bins they straddle, in
            // sixteenths per axis: truncation used to alias the code
            // lattice into texture even at 256, and parked the whole
            // cloud half a bin off the positions the projection - and
            // with it every marker and graticule target - reports.
            const int size = CodeGridSize;
            const int span = size * 16;
            for (; pixel < rowEnd; pixel += static_cast<std::ptrdiff_t>(4) * stride) {
                const int b = pixel[0], g = pixel[1], r = pixel[2];
                const int64_t cbRaw = matrix.cbFromR * r + matrix.cbFromG * g + matrix.cbFromB * b;
                const int64_t crRaw = matrix.crFromR * r + matrix.crFromG * g + matrix.crFromB * b;
                const int cbPosition = std::clamp(static_cast<int>(cbRaw * span >> 16) + span / 2, 0, span - 17);
                const int crPosition = std::clamp(static_cast<int>(crRaw * span >> 16) + span / 2, 0, span - 17);
                const int cbBin = cbPosition >> 4;
                const int crBin = crPosition >> 4;
                const uint32_t cbHigh = static_cast<uint32_t>(cbPosition & 15);
                const uint32_t crHigh = static_cast<uint32_t>(crPosition & 15);
                const uint32_t cbLow = 16 - cbHigh;
                const uint32_t crLow = 16 - crHigh;
                uint32_t* upper = m_bins.data() + static_cast<std::size_t>(size - 1 - crBin) * size + cbBin;
                uint32_t* above = upper - size;  // cr_bin + 1 is one row up
                upper[0] += cbLow * crLow;
                upper[1] += cbHigh * crLow;
                above[0] += cbLow * crHigh;
                above[1] += cbHigh * crHigh;
                ++sampleCount;
            }
        }
    }
    mapBinsToImage(sampleCount);
}

NormalizedPoint Vectorscope::project(const FloatColor& color) const
{
    // Floating point throughout: markers need sub-bin positions.
    const ChromaCoefficients& matrix = coefficientsFor(m_settings.matrix);
    const float cb = (static_cast<float>(matrix.cbFromR) * color.r + static_cast<float>(matrix.cbFromG) * color.g +
                      static_cast<float>(matrix.cbFromB) * color.b) /
                         256.0f +
                     128.0f;
    const float cr = (static_cast<float>(matrix.crFromR) * color.r + static_cast<float>(matrix.crFromG) * color.g +
                      static_cast<float>(matrix.crFromB) * color.b) /
                         256.0f +
                     128.0f;
    return NormalizedPoint{cb / 255.0f, (255.0f - cr) / 255.0f};
}

void Vectorscope::rebuildTintTable()
{
    // Each bin is shown in the hue it represents: invert the chroma
    // transform at a fixed mid luma. BT.601 inverse coefficients are close
    // enough for display tinting in both matrix modes.
    constexpr float DisplayLuma = 160.0f;
    // Chroma is exaggerated for display: the trace paints the hue a bin
    // represents, not a colorimetric reproduction, and at true saturation
    // the cloud reads as washed-out pastel.
    constexpr float SaturationBoost = 1.7f;
    const float toChroma = 256.0f / static_cast<float>(m_imageSize);
    for (int py = 0; py < m_imageSize; ++py) {
        for (int px = 0; px < m_imageSize; ++px) {
            const float cb = (static_cast<float>(px) * toChroma - 128.0f) * SaturationBoost;
            const float cr = (static_cast<float>(m_imageSize - 1 - py) * toChroma - 128.0f) * SaturationBoost;
            const auto toByte = [](float value) { return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f)); };
            uint8_t* tint = m_tint.data() + (static_cast<std::size_t>(py) * m_imageSize + px) * 3;
            tint[0] = toByte(DisplayLuma + 1.402f * cr);
            tint[1] = toByte(DisplayLuma - 0.344f * cb - 0.714f * cr);
            tint[2] = toByte(DisplayLuma + 1.772f * cb);
        }
    }
}

void Vectorscope::mapBinsToImage(uint64_t sampleCount)
{
    smoothCodeGrid();
    adaptiveDensityEstimate(sampleCount);
    int densitySize = CodeGridSize;
    const float* densities = upsampleToImage(densitySize);
    renderTrace(densities, densitySize, sampleCount);
}

void Vectorscope::smoothCodeGrid()
{
    // A half-strength 3x3 binomial takes the worst speckle out of the
    // cloud without softening it: the fractional splat already spreads
    // each sample as a tent, so a full binomial on top reads as gaussian
    // blur at large pane sizes.
    halfBinomial(m_bins.data(), CodeGridSize, m_smoothed.data());
}

void Vectorscope::adaptiveDensityEstimate(uint64_t sampleCount)
{
    // Adaptive density estimation: where the cloud is dim, counts are
    // low and the photograph's own chroma quantization shows through as
    // code-level ripple that the log display amplifies - measured on a
    // production frame, the source JPEG's code populations are bumpier
    // than the framebuffer's, so this is content, not the pipeline, and
    // cannot be corrected, only estimated. The dense body has the
    // statistics to support full sharpness and keeps it; two extra
    // binomial passes build a wide estimate, and each cell blends
    // toward it as its density falls.
    std::vector<float> wide(m_smoothed);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> next(wide.size(), 0.0f);
        for (int py = 0; py < CodeGridSize; ++py) {
            for (int px = 0; px < CodeGridSize; ++px) {
                const auto at = [&](int y, int x) -> float {
                    if (y < 0 || y >= CodeGridSize || x < 0 || x >= CodeGridSize) {
                        return 0.0f;
                    }
                    return wide[static_cast<std::size_t>(y) * CodeGridSize + x];
                };
                const auto row = [&](int y) { return at(y, px - 1) + 2.0f * at(y, px) + at(y, px + 1); };
                next[static_cast<std::size_t>(py) * CodeGridSize + px] =
                    (row(py - 1) + 2.0f * row(py) + row(py + 1)) / 16.0f;
            }
        }
        wide.swap(next);
    }
    float densestNarrow = 0.0f;
    for (const float count : m_smoothed) {
        densestNarrow = std::max(densestNarrow, count);
    }
    if (densestNarrow > 0.0f) {
        // One nominal sample's weight, scaled to the actual sample
        // count so the trace stays invariant to the sampling stride.
        const float sampleFloor = static_cast<float>(sampleCount) * static_cast<float>(256.0 / ReferenceSampleCount);
        for (std::size_t i = 0; i < m_smoothed.size(); ++i) {
            // The wide estimate may redistribute, never amplify:
            // next to the razor-thin ridges dense content forms, the
            // binomial passes leak the ridge's mass outward, and an
            // uncapped blend would paint a bright halo hundreds of
            // times above the cells' own evidence. A floor of about
            // one sample's weight keeps empty tail cells glowing.
            const float capped = std::min(wide[i], 3.0f * m_smoothed[i] + sampleFloor);
            const float density = capped / densestNarrow;
            const float t = std::clamp((density - 0.001f) / (0.02f - 0.001f), 0.0f, 1.0f);
            const float blend = t * t * (3.0f - 2.0f * t);  // smoothstep
            m_smoothed[i] = blend * m_smoothed[i] + (1.0f - blend) * capped;
        }
    }
}

const float* Vectorscope::upsampleToImage(int& densitySize)
{
    // A finer display image interpolates the code grid bilinearly and
    // settles the diamond artifacts with a light binomial at display
    // resolution: unlike a cubic, this can neither ring at density
    // cliffs nor staircase along thin diagonal ridges.
    if (m_imageSize <= CodeGridSize) {
        densitySize = CodeGridSize;
        return m_smoothed.data();
    }
    const auto at = [&](int y, int x) -> float {
        y = std::clamp(y, 0, CodeGridSize - 1);
        x = std::clamp(x, 0, CodeGridSize - 1);
        return m_smoothed[static_cast<std::size_t>(y) * CodeGridSize + x];
    };
    const float step = static_cast<float>(CodeGridSize) / static_cast<float>(m_imageSize);
    for (int py = 0; py < m_imageSize; ++py) {
        const float sy = (static_cast<float>(py) + 0.5f) * step - 0.5f;
        const int baseY = static_cast<int>(std::floor(sy));
        const float ty = sy - static_cast<float>(baseY);
        for (int px = 0; px < m_imageSize; ++px) {
            const float sx = (static_cast<float>(px) + 0.5f) * step - 0.5f;
            const int baseX = static_cast<int>(std::floor(sx));
            const float tx = sx - static_cast<float>(baseX);
            const float top = at(baseY, baseX) * (1.0f - tx) + at(baseY, baseX + 1) * tx;
            const float bottom = at(baseY + 1, baseX) * (1.0f - tx) + at(baseY + 1, baseX + 1) * tx;
            m_upsampled[static_cast<std::size_t>(py) * m_imageSize + px] = top * (1.0f - ty) + bottom * ty;
        }
    }
    std::vector<float> settled(m_upsampled.size(), 0.0f);
    halfBinomial(m_upsampled.data(), m_imageSize, settled.data());
    m_upsampled.swap(settled);
    densitySize = m_imageSize;
    return m_upsampled.data();
}

void Vectorscope::renderTrace(const float* densities, int densitySize, uint64_t sampleCount)
{
    float densest = 0.0f;
    const std::size_t densityCount = static_cast<std::size_t>(densitySize) * densitySize;
    for (std::size_t i = 0; i < densityCount; ++i) {
        densest = std::max(densest, densities[i]);
    }

    // Each sample contributes 256 weight units (the splat's sixteenths
    // squared); the normalization divides them back out so the gain
    // keeps its calibrated feel.
    const double perSampleScale =
        sampleCount > 0 ? ReferenceSampleCount / (static_cast<double>(sampleCount) * 256.0) : 0.0;
    const double gain = static_cast<double>(m_settings.gain) * perSampleScale;
    const double logCeiling = densest > 0.0f ? std::log1p(densest * gain) : 0.0;
    const double intensityScale = logCeiling > 0.0 ? 1.0 / logCeiling : 0.0;
    const bool linear = m_settings.response == TraceResponse::Linear;
    // The linear response emulates a phosphor: exposure saturates
    // exponentially, so the densest mass just reaches full glow at the
    // default gain and faint spread stays honestly faint. The gain acts
    // as the phosphor's sensitivity.
    const double phosphorRate =
        densest > 0.0f ? static_cast<double>(m_settings.gain) / static_cast<double>(densest) : 0.0;

    // Where the beam parks, phosphor overexposes toward white: past the
    // bloom knee the tint desaturates with density. The white-hot core
    // over neutral content doubles as an at-a-glance neutrality check.
    const float bloomKnee = linear ? 0.72f : 0.88f;
    const float bloomScale = 0.9f / (1.0f - bloomKnee);

    uint8_t* out = m_image.rgba.data();
    const uint8_t* tint = m_tint.data();
    for (std::size_t i = 0; i < densityCount; ++i, out += 4, tint += 3) {
        const float count = densities[i];
        if (count <= 0.0f) {
            out[0] = out[1] = out[2] = 0;
            out[3] = 255;
            continue;
        }
        float brightness;
        if (linear) {
            brightness = static_cast<float>(1.0 - std::exp(-static_cast<double>(count) * phosphorRate));
        } else {
            // The gamma lifts the mid-density body of the cloud:
            // normalizing to the densest bin pushes everything else
            // down, and a linear ramp leaves the trace dim at any gain.
            const float normalized = static_cast<float>(std::log1p(static_cast<double>(count) * gain) * intensityScale);
            brightness = applyMidDensityGamma(normalized);
        }
        const float whiteness = std::clamp((brightness - bloomKnee) * bloomScale, 0.0f, 0.9f);
        out[0] = static_cast<uint8_t>(
            (static_cast<float>(tint[0]) + (255.0f - static_cast<float>(tint[0])) * whiteness) * brightness);
        out[1] = static_cast<uint8_t>(
            (static_cast<float>(tint[1]) + (255.0f - static_cast<float>(tint[1])) * whiteness) * brightness);
        out[2] = static_cast<uint8_t>(
            (static_cast<float>(tint[2]) + (255.0f - static_cast<float>(tint[2])) * whiteness) * brightness);
        out[3] = 255;
    }
    ++m_image.sequence;
}

}  // namespace sidescopes
