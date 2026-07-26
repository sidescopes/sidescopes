#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/frame.h"
#include "core/scopes/chunk_scratch.h"
#include "core/scopes/graticule.h"
#include "core/scopes/sampling.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

inline constexpr int DefaultNeutralSize = 256;

/// The largest plane the cloud is accumulated at. Every sample is splatted over
/// three pixels, so past this the plane resolves structure the splat has already
/// spread - and a square image is memory in both axes.
inline constexpr int MaximumNeutralSize = 1024;

/// How wide a slice of the a*/b* plane the scope shows, as the half-extent in
/// CIELAB units from neutral to each edge. Everyday casts sit inside Normal;
/// Fine magnifies a subtle one, Wide keeps a strong cast on screen.
enum class NeutralRange
{
    Fine,    ///< +-20
    Normal,  ///< +-40
    Wide,    ///< +-80
};

/// Samples the neutral scope needs in each of its bins, and the one budget that
/// is also capped: its plane follows its pane, so its bins grow to a million,
/// but it converts every sample to L*a*b* and an unbounded pass over a whole
/// display measured 69 ms. So it takes the smaller of this and SampleBudget,
/// which leaves a large plane costing what it does today and makes the default
/// one cheaper. Calibrated over a whole 3456x2234 display at the default plane:
/// the pass falls from 35 ms to 14, the cloud moves by a mean of 0.70 of 255,
/// and the average-cast dot - the reading anyone takes off this scope - moves
/// by 0.02 of the 160 a*b* units its widest range spans.
inline constexpr int NeutralMinSamplesPerBin = 24;

struct NeutralSettings
{
    /// Brightness applied to the near-neutral cloud's density.
    float gain = 1.0f;
    /// Sample every Nth pixel horizontally and vertically (1..8).
    int samplingStride = 1;
    /// A pixel joins the cloud when its chroma C* is at or below this.
    float neutralChroma = 12.0f;
    NeutralRange range = NeutralRange::Normal;
    /// Display image resolution per axis.
    int size = DefaultNeutralSize;
};

/// One label on the plane's edge, in normalized image coordinates. The host
/// styles and places the text; this states only where and what.
struct NeutralLabel
{
    NormalizedPoint at;
    std::string text;
};

/// The neutral graticule: the crosshair through neutral, magnitude rings, and
/// the four edge labels. Pure geometry in normalized coordinates, derived from
/// the configured range alone, so the overlay never disagrees with the plane.
struct NeutralGraticule
{
    std::vector<GraticuleLine> lines;
    std::vector<GraticuleCircle> circles;
    std::vector<NeutralLabel> labels;
};

/// The neutral / white-balance-cast scope: the region's colour balance as an
/// offset from neutral on the CIELAB a* (green-magenta) and b* (blue-yellow)
/// axes. A bright dot marks the overall average cast; a faint cloud shows
/// where the near-neutral pixels actually sit, so a tinted set of greys reads
/// at a glance. Not thread-safe; a single analysis thread owns each instance.
class Neutral
{
public:
    static constexpr int CodeGridSize = DefaultNeutralSize;

    Neutral();

    /// Applies @p settings, clamping each value to its documented range.
    void configure(const NeutralSettings& settings);

    /// Folds a frame region into the average and the near-neutral cloud.
    void accumulate(const FrameView& frame, IntRect region);

    /// Takes the room a split pass needs for its per-chunk bins from the
    /// host's shared arena rather than from its own. The whole stack then
    /// holds one arena instead of a set each; an engine never told keeps its
    /// own, which is what a test or a benchmark does.
    void lendScratch(ChunkScratch::Lender lender, const void* context)
    {
        m_scratch.lendFrom(lender, context);
    }

    /// The composed scope image.
    [[nodiscard]] const ScopeImage& image() const
    {
        return m_image;
    }

    /// Where a colour lands on the plane, in normalized image coordinates.
    /// The cursor marker and the graticule both go through this projection,
    /// which guarantees the overlays agree with the trace.
    [[nodiscard]] NormalizedPoint project(const FloatColor& color) const;

    /// The plane's half-extent in CIELAB units for the configured range.
    [[nodiscard]] float axisRange() const;

    /// The overall average cast, projected; the centre when nothing was seen.
    [[nodiscard]] NormalizedPoint averagePoint() const
    {
        return m_average;
    }

    /// How many near-neutral pixels the last accumulate folded into the cloud.
    [[nodiscard]] uint64_t neutralCount() const
    {
        return m_neutralCount;
    }

private:
    /// Brings the cloud and image up to the configured size, allocating them
    /// if this is the first pass. Nothing else may allocate them: an engine
    /// that has never accumulated holds no cloud at all.
    void ensureBuffers();
    void resize(int size);
    void renderImage();

    /// The running totals a chunk accumulates, merged by plain addition.
    struct ChromaTotals
    {
        int64_t sumA = 0;
        int64_t sumB = 0;
        uint64_t count = 0;
        uint64_t neutral = 0;
    };

    void scatterRows(const FrameView& frame, IntRect region, const SampleGrid& grid, int rowBegin, int rowEnd,
                     uint32_t* cloud, ChromaTotals& totals) const;
    void splatInto(uint32_t* cloud, NormalizedPoint at) const;
    void drawCloud();
    void drawDot();
    void blendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, float alpha);
    [[nodiscard]] NormalizedPoint projectAb(float a, float b) const;

    NeutralSettings m_settings;
    int m_imageSize = DefaultNeutralSize;
    float m_axisRange = 40.0f;
    // m_imageSize x m_imageSize near-neutral density, in quarter units so the
    // splat's 1, 1/2 and 1/4 weights are whole numbers. Integers because a
    // chunked accumulate has to merge bit-exactly however many chunks it used,
    // and float addition is not associative.
    std::vector<uint32_t> m_cloud;
    // Per-chunk private clouds, merged into m_cloud by integer addition.
    ChunkScratch m_scratch;
    NormalizedPoint m_average{0.5f, 0.5f};
    uint64_t m_neutralCount = 0;
    bool m_hasData = false;
    ScopeImage m_image;
};

/// Builds the neutral graticule: crosshair, two magnitude rings, and the four
/// edge labels. The geometry is the same at every range - the range only sets
/// what CIELAB values the edges stand for, which the projection already holds.
[[nodiscard]] NeutralGraticule buildNeutralGraticule();

}  // namespace sidescopes
