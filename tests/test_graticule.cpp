#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "core/scopes/graticule.h"
#include "core/scopes/vectorscope.h"

namespace sidescopes {

TEST_CASE("Vectorscope graticule targets sit exactly on the projections")
{
    Vectorscope scope;
    const VectorscopeGraticule graticule = buildVectorscopeGraticule(scope);

    // Six labeled primaries and six unlabeled 100% secondaries.
    const auto primaries =
        std::count_if(graticule.targets.begin(), graticule.targets.end(), [](const auto& t) { return t.primary; });
    CHECK(primaries == 6);
    CHECK(graticule.targets.size() == 12);

    const NormalizedPoint red75 = scope.project(FloatColor{191.0f, 0.0f, 0.0f});
    const auto redTarget =
        std::find_if(graticule.targets.begin(), graticule.targets.end(), [](const auto& t) { return t.label == "R"; });
    REQUIRE(redTarget != graticule.targets.end());
    CHECK(redTarget->center.x == red75.x);
    CHECK(redTarget->center.y == red75.y);
}

TEST_CASE("Vectorscope graticule targets follow the matrix")
{
    Vectorscope bt601;
    Vectorscope bt709;
    VectorscopeSettings settings;
    settings.matrix = ChromaMatrix::Bt601;
    bt601.configure(settings);
    settings.matrix = ChromaMatrix::Bt709;
    bt709.configure(settings);

    const auto findRed = [](const VectorscopeGraticule& graticule) {
        return std::find_if(graticule.targets.begin(), graticule.targets.end(),
                            [](const auto& t) { return t.label == "R"; })
            ->center;
    };
    const NormalizedPoint red601 = findRed(buildVectorscopeGraticule(bt601));
    const NormalizedPoint red709 = findRed(buildVectorscopeGraticule(bt709));
    CHECK(red601.x != red709.x);  // the overlay moves with the data
}

TEST_CASE("Vectorscope graticule includes the skin-tone line on the ring")
{
    Vectorscope scope;
    const VectorscopeGraticule graticule = buildVectorscopeGraticule(scope);

    const auto skinLine = std::find_if(graticule.lines.begin(), graticule.lines.end(),
                                       [](const auto& line) { return line.stroke == GraticuleStroke::SkinTone; });
    REQUIRE(skinLine != graticule.lines.end());

    // The line starts at the center and ends on the outer ring.
    CHECK(skinLine->from.x == 0.5f);
    CHECK(skinLine->from.y == 0.5f);
    const float dx = skinLine->to.x - 0.5f;
    const float dy = skinLine->to.y - 0.5f;
    CHECK(std::sqrt(dx * dx + dy * dy) == Catch::Approx(0.5).margin(1e-4));

    // And it points through the projected reference skin tone.
    const NormalizedPoint skin = scope.project(FloatColor{203.0f, 171.0f, 153.0f});
    const float cross = dx * (skin.y - 0.5f) - dy * (skin.x - 0.5f);
    CHECK(cross == Catch::Approx(0.0).margin(1e-4));
}

TEST_CASE("Waveform scale has ten percent steps with majors at the anchors")
{
    const auto lines = buildWaveformScale();
    REQUIRE(lines.size() == 11);
    CHECK(lines.front().levelPercent == 100.0f);
    CHECK(lines.front().y == 0.0f);
    CHECK(lines.back().levelPercent == 0.0f);
    CHECK(lines.back().y == 1.0f);
    CHECK(lines.front().major);
    CHECK(lines[5].major);  // 50%
    CHECK_FALSE(lines[1].major);
    CHECK(lines[1].label == "90");
}

}  // namespace sidescopes
