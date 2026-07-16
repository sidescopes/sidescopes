#include <cmath>
#include <numbers>

#include "core/scopes/graticule.h"
#include "core/scopes/vectorscope.h"

// The vectorscope graticule lives with the vectorscope engine, not in core's
// shared graticule.cpp: it is the one builder that calls Vectorscope::project,
// so keeping it here lets every module link self-contained. A module that draws
// only the waveform scale never pulls a reference to the vectorscope engine.

namespace sidescopes {
namespace {

constexpr NormalizedPoint Center{0.5f, 0.5f};

// The reference skin color whose projection anchors the skin-tone line.
constexpr FloatColor SkinToneReference{203.0f, 171.0f, 153.0f};

// Below this the skin reference projects essentially onto the neutral
// center and has no stable direction to draw the line along.
constexpr float SkinLineMinLength = 1e-6f;

struct BarColor
{
    FloatColor color;
    const char* label;
};

constexpr BarColor BarColors[6] = {
    {{191.0f, 0.0f, 0.0f}, "R"},    {{191.0f, 191.0f, 0.0f}, "Yl"}, {{0.0f, 191.0f, 0.0f}, "G"},
    {{0.0f, 191.0f, 191.0f}, "Cy"}, {{0.0f, 0.0f, 191.0f}, "B"},    {{191.0f, 0.0f, 191.0f}, "Mg"},
};

}  // namespace

VectorscopeGraticule buildVectorscopeGraticule(const Vectorscope& scope)
{
    VectorscopeGraticule graticule;

    // Crosshair.
    graticule.lines.push_back({{0.5f, 0.0f}, {0.5f, 1.0f}, GraticuleStroke::Grid});
    graticule.lines.push_back({{0.0f, 0.5f}, {1.0f, 0.5f}, GraticuleStroke::Grid});

    // Half and full rings, with tick marks every 30 degrees on the outer.
    graticule.circles.push_back({Center, 0.25f, GraticuleStroke::Grid});
    graticule.circles.push_back({Center, 0.5f, GraticuleStroke::Grid});
    for (int tick = 0; tick < 12; ++tick) {
        const float angle = static_cast<float>(tick) * std::numbers::pi_v<float> / 6.0f;
        const float dx = std::cos(angle);
        const float dy = std::sin(angle);
        graticule.lines.push_back(
            {{0.5f + dx * 0.48f, 0.5f + dy * 0.48f}, {0.5f + dx * 0.5f, 0.5f + dy * 0.5f}, GraticuleStroke::GridMajor});
    }

    // Color targets at 75% (primary, labeled) and 100% (secondary).
    for (const BarColor& bar : BarColors) {
        graticule.targets.push_back({scope.project(bar.color), true, bar.label});
        const FloatColor full{bar.color.r > 0.0f ? 255.0f : 0.0f, bar.color.g > 0.0f ? 255.0f : 0.0f,
                              bar.color.b > 0.0f ? 255.0f : 0.0f};
        graticule.targets.push_back({scope.project(full), false, ""});
    }

    // Skin-tone line: from the center through the reference skin color's
    // projection, extended to the ring.
    const NormalizedPoint skin = scope.project(SkinToneReference);
    const float dx = skin.x - Center.x;
    const float dy = skin.y - Center.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length > SkinLineMinLength) {
        const float scale = 0.5f / length;
        graticule.lines.push_back({Center, {Center.x + dx * scale, Center.y + dy * scale}, GraticuleStroke::SkinTone});
    }

    return graticule;
}

}  // namespace sidescopes
