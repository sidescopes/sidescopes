#pragma once

#include <string>
#include <vector>

#include "core/scopes/scope_types.h"

namespace sidescopes {

class Vectorscope;

// Graticule geometry as plain data in normalized scope coordinates, built
// from the same projection the engines use — which is what makes it
// impossible for the overlay to disagree with the trace. The UI decides
// stroke widths, colors, and fonts; this decides positions.

enum class GraticuleStroke
{
    Grid,
    GridMajor,
    Accent,
    SkinTone
};

struct GraticuleLine
{
    NormalizedPoint from;
    NormalizedPoint to;
    GraticuleStroke stroke = GraticuleStroke::Grid;
};

struct GraticuleCircle
{
    NormalizedPoint center;
    float radius = 0.0f;  ///< As a fraction of the scope width.
    GraticuleStroke stroke = GraticuleStroke::Grid;
};

/// A chroma target box: primary boxes mark 75% color bars, secondary the
/// 100% positions.
struct GraticuleTarget
{
    NormalizedPoint center;
    bool primary = true;
    std::string label;  ///< Empty for the secondary boxes.
};

struct VectorscopeGraticule
{
    std::vector<GraticuleLine> lines;
    std::vector<GraticuleCircle> circles;
    std::vector<GraticuleTarget> targets;
};

struct WaveformScaleLine
{
    float levelPercent = 0.0f;  ///< 0 at black, 100 at white.
    float y = 0.0f;             ///< Normalized vertical position.
    bool major = false;
    std::string label;
};

/// Builds the classic vectorscope graticule for the scope's current matrix:
/// crosshair, two rings with hour ticks, primary/secondary color targets, and
/// the skin-tone line.
[[nodiscard]] VectorscopeGraticule buildVectorscopeGraticule(const Vectorscope& scope);

/// Horizontal scale lines every 10%, majors at 0/50/100.
[[nodiscard]] std::vector<WaveformScaleLine> buildWaveformScale();

}  // namespace sidescopes
