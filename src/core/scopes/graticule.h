#pragma once

#include <string>
#include <vector>

#include "core/scopes/scope_types.h"
#include "core/scopes/vectorscope.h"

namespace sidescopes {

// Graticule geometry as plain data in normalized scope coordinates, built
// from the same projection the engines use — which is what makes it
// impossible for the overlay to disagree with the trace. The UI decides
// stroke widths, colors, and fonts; this decides positions.

enum class GraticuleStroke { Grid, GridMajor, Accent, SkinTone };

struct GraticuleLine {
    NormalizedPoint from;
    NormalizedPoint to;
    GraticuleStroke stroke = GraticuleStroke::Grid;
};

struct GraticuleCircle {
    NormalizedPoint center;
    float radius = 0.0f;  // as a fraction of the scope width
    GraticuleStroke stroke = GraticuleStroke::Grid;
};

// A chroma target box: primary boxes mark 75% color bars, secondary the
// 100% positions.
struct GraticuleTarget {
    NormalizedPoint center;
    bool primary = true;
    std::string label;  // empty for the secondary boxes
};

struct VectorscopeGraticule {
    std::vector<GraticuleLine> lines;
    std::vector<GraticuleCircle> circles;
    std::vector<GraticuleTarget> targets;
};

struct WaveformScaleLine {
    float level_percent = 0.0f;  // 0 at black, 100 at white
    float y = 0.0f;              // normalized vertical position
    bool major = false;
    std::string label;
};

// Builds the classic vectorscope graticule for the scope's current matrix:
// crosshair, two rings with hour ticks, primary/secondary color targets, and
// the skin-tone line.
VectorscopeGraticule BuildVectorscopeGraticule(const Vectorscope& scope);

// Horizontal scale lines every 10%, majors at 0/50/100.
std::vector<WaveformScaleLine> BuildWaveformScale();

}  // namespace sidescopes
