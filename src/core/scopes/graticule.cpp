#include "core/scopes/graticule.h"

#include <cmath>

namespace sidescopes {
namespace {

constexpr NormalizedPoint Center{0.5f, 0.5f};

// The reference skin color whose projection anchors the skin-tone line.
constexpr FloatColor SkinToneReference{203.0f, 171.0f, 153.0f};

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
        const float angle = static_cast<float>(tick) * 3.14159265f / 6.0f;
        const float dx = std::cos(angle);
        const float dy = std::sin(angle);
        graticule.lines.push_back(
            {{0.5f + dx * 0.48f, 0.5f + dy * 0.48f}, {0.5f + dx * 0.5f, 0.5f + dy * 0.5f}, GraticuleStroke::GridMajor});
    }

    // Color targets at 75% (primary, labeled) and 100% (secondary).
    for (const BarColor& bar : BarColors) {
        if (const auto at75 = scope.project(bar.color)) {
            graticule.targets.push_back({*at75, true, bar.label});
        }
        const FloatColor full{bar.color.r > 0.0f ? 255.0f : 0.0f, bar.color.g > 0.0f ? 255.0f : 0.0f,
                              bar.color.b > 0.0f ? 255.0f : 0.0f};
        if (const auto at100 = scope.project(full)) {
            graticule.targets.push_back({*at100, false, ""});
        }
    }

    // Skin-tone line: from the center through the reference skin color's
    // projection, extended to the ring.
    if (const auto skin = scope.project(SkinToneReference)) {
        const float dx = skin->x - Center.x;
        const float dy = skin->y - Center.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 1e-6f) {
            const float scale = 0.5f / length;
            graticule.lines.push_back(
                {Center, {Center.x + dx * scale, Center.y + dy * scale}, GraticuleStroke::SkinTone});
        }
    }
    return graticule;
}

std::vector<WaveformScaleLine> buildWaveformScale()
{
    std::vector<WaveformScaleLine> lines;
    for (int step = 0; step <= 10; ++step) {
        WaveformScaleLine line;
        line.levelPercent = static_cast<float>(100 - step * 10);
        line.y = static_cast<float>(step) / 10.0f;
        line.major = step % 5 == 0;
        line.label = std::to_string(100 - step * 10);
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace sidescopes
