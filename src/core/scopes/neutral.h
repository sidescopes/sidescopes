#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/frame.h"
#include "core/scopes/graticule.h"
#include "core/scopes/scope_types.h"

namespace sidescopes {

inline constexpr int DefaultNeutralSize = 256;

/// How wide a slice of the a*/b* plane the scope shows, as the half-extent in
/// CIELAB units from neutral to each edge. Everyday casts sit inside Normal;
/// Fine magnifies a subtle one, Wide keeps a strong cast on screen.
enum class NeutralRange
{
    Fine,    ///< +-20
    Normal,  ///< +-40
    Wide,    ///< +-80
};

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
    void resize(int size);
    void renderImage();
    void splatNeutral(NormalizedPoint at);
    void drawCloud();
    void drawDot();
    void blendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, float alpha);
    [[nodiscard]] NormalizedPoint projectAb(float a, float b) const;

    NeutralSettings m_settings;
    int m_imageSize = DefaultNeutralSize;
    float m_axisRange = 40.0f;
    std::vector<float> m_cloud;  // m_imageSize x m_imageSize near-neutral density
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
