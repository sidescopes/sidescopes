#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "core/scopes/graticule.h"

namespace sidescopes {

TEST_CASE("Vectorscope graticule targets sit exactly on the projections") {
    Vectorscope scope;
    const VectorscopeGraticule graticule = BuildVectorscopeGraticule(scope);

    // Six labeled primaries and six unlabeled 100% secondaries.
    const auto primaries = std::count_if(graticule.targets.begin(), graticule.targets.end(),
                                         [](const auto& t) { return t.primary; });
    CHECK(primaries == 6);
    CHECK(graticule.targets.size() == 12);

    const auto red75 = scope.Project(FloatColor{191.0f, 0.0f, 0.0f});
    REQUIRE(red75.has_value());
    const auto red_target = std::find_if(graticule.targets.begin(), graticule.targets.end(),
                                         [](const auto& t) { return t.label == "R"; });
    REQUIRE(red_target != graticule.targets.end());
    CHECK(red_target->center.x == red75->x);
    CHECK(red_target->center.y == red75->y);
}

TEST_CASE("Vectorscope graticule targets follow the matrix") {
    Vectorscope bt601;
    Vectorscope bt709;
    VectorscopeSettings settings;
    settings.matrix = ChromaMatrix::Bt601;
    bt601.Configure(settings);
    settings.matrix = ChromaMatrix::Bt709;
    bt709.Configure(settings);

    const auto find_red = [](const VectorscopeGraticule& graticule) {
        return std::find_if(graticule.targets.begin(), graticule.targets.end(),
                            [](const auto& t) { return t.label == "R"; })
            ->center;
    };
    const NormalizedPoint red601 = find_red(BuildVectorscopeGraticule(bt601));
    const NormalizedPoint red709 = find_red(BuildVectorscopeGraticule(bt709));
    CHECK(red601.x != red709.x);  // the overlay moves with the data
}

TEST_CASE("Vectorscope graticule includes the skin-tone line on the ring") {
    Vectorscope scope;
    const VectorscopeGraticule graticule = BuildVectorscopeGraticule(scope);

    const auto skin_line =
        std::find_if(graticule.lines.begin(), graticule.lines.end(),
                     [](const auto& line) { return line.stroke == GraticuleStroke::SkinTone; });
    REQUIRE(skin_line != graticule.lines.end());

    // The line starts at the center and ends on the outer ring.
    CHECK(skin_line->from.x == 0.5f);
    CHECK(skin_line->from.y == 0.5f);
    const float dx = skin_line->to.x - 0.5f;
    const float dy = skin_line->to.y - 0.5f;
    CHECK(std::sqrt(dx * dx + dy * dy) == Catch::Approx(0.5).margin(1e-4));

    // And it points through the projected reference skin tone.
    const auto skin = scope.Project(FloatColor{203.0f, 171.0f, 153.0f});
    REQUIRE(skin.has_value());
    const float cross = dx * (skin->y - 0.5f) - dy * (skin->x - 0.5f);
    CHECK(cross == Catch::Approx(0.0).margin(1e-4));
}

TEST_CASE("Waveform scale has ten percent steps with majors at the anchors") {
    const auto lines = BuildWaveformScale();
    REQUIRE(lines.size() == 11);
    CHECK(lines.front().level_percent == 100.0f);
    CHECK(lines.front().y == 0.0f);
    CHECK(lines.back().level_percent == 0.0f);
    CHECK(lines.back().y == 1.0f);
    CHECK(lines.front().major);
    CHECK(lines[5].major);  // 50%
    CHECK_FALSE(lines[1].major);
    CHECK(lines[1].label == "90");
}

}  // namespace sidescopes
