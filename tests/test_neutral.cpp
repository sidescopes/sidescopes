// Unit tests for the neutral / white-balance-cast engine: the projection that
// places a colour on the CIELAB a*/b* plane, the average and near-neutral
// counting the accumulate produces, the range that sets the plane's extent, and
// the graticule geometry. All are pure functions of the frame and settings, so
// they run on every platform.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>

#include "core/scopes/neutral.h"
#include "scope_image.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

TEST_CASE("Neutral projects a grey to the centre of the plane")
{
    Neutral scope;
    const NormalizedPoint point = scope.project(FloatColor{128, 128, 128});
    CHECK(point.x == Catch::Approx(0.5f).margin(0.02f));
    CHECK(point.y == Catch::Approx(0.5f).margin(0.02f));
}

TEST_CASE("Neutral projects warm right, cool left, magenta up, green down")
{
    Neutral scope;
    // b* (blue-yellow) is the temperature axis: yellow to the right, blue left.
    CHECK(scope.project(FloatColor{220, 200, 120}).x > 0.5f);
    CHECK(scope.project(FloatColor{120, 150, 220}).x < 0.5f);
    // a* (green-magenta) is the tint axis, magenta up (smaller y), green down.
    CHECK(scope.project(FloatColor{210, 120, 180}).y < 0.5f);
    CHECK(scope.project(FloatColor{120, 200, 120}).y > 0.5f);
}

TEST_CASE("Neutral averages a solid grey frame onto the centre")
{
    TestFrame frame(16, 16, 255);
    frame.fill(0, 16, Color{130, 130, 130});

    Neutral scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 16, 16});

    CHECK(scope.averagePoint().x == Catch::Approx(0.5f).margin(0.02f));
    CHECK(scope.averagePoint().y == Catch::Approx(0.5f).margin(0.02f));
    // A neutral grey is near-neutral, so every sampled pixel joins the cloud.
    CHECK(scope.neutralCount() == 256u);
    // The average dot is drawn, so the image has lit pixels.
    CHECK(brightestPixel(scope.image()) != std::pair<int, int>{-1, -1});
}

TEST_CASE("Neutral excludes a saturated frame from the neutral cloud")
{
    TestFrame frame(16, 16, 255);
    frame.fill(0, 16, Color{220, 40, 40});  // saturated red, well past the threshold

    Neutral scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 16, 16});

    CHECK(scope.neutralCount() == 0u);
    // The average still lands where the colour itself projects.
    const NormalizedPoint expected = scope.project(FloatColor{220, 40, 40});
    CHECK(scope.averagePoint().x == Catch::Approx(expected.x));
    CHECK(scope.averagePoint().y == Catch::Approx(expected.y));
}

TEST_CASE("Neutral range sets the plane's half-extent")
{
    Neutral scope;
    NeutralSettings settings;
    settings.range = NeutralRange::Fine;
    scope.configure(settings);
    CHECK(scope.axisRange() == 20.0f);
    settings.range = NeutralRange::Normal;
    scope.configure(settings);
    CHECK(scope.axisRange() == 40.0f);
    settings.range = NeutralRange::Wide;
    scope.configure(settings);
    CHECK(scope.axisRange() == 80.0f);
}

TEST_CASE("Neutral magnifies a cast at a finer range")
{
    const FloatColor warm{190, 185, 165};  // a subtle warm cast, clamps at neither range

    Neutral fine;
    NeutralSettings fineSettings;
    fineSettings.range = NeutralRange::Fine;
    fine.configure(fineSettings);

    Neutral wide;
    NeutralSettings wideSettings;
    wideSettings.range = NeutralRange::Wide;
    wide.configure(wideSettings);

    const float fineOffset = std::abs(fine.project(warm).x - 0.5f);
    const float wideOffset = std::abs(wide.project(warm).x - 0.5f);
    CHECK(fineOffset > wideOffset);
}

TEST_CASE("Neutral clamps an extreme colour onto the plane")
{
    Neutral scope;
    NeutralSettings settings;
    settings.range = NeutralRange::Fine;
    scope.configure(settings);

    const NormalizedPoint point = scope.project(FloatColor{255, 240, 0});  // very yellow
    CHECK(point.x >= 0.0f);
    CHECK(point.x <= 1.0f);
    CHECK(point.y >= 0.0f);
    CHECK(point.y <= 1.0f);
}

TEST_CASE("Neutral graticule is the crosshair, two rings, and four labels")
{
    const NeutralGraticule graticule = buildNeutralGraticule();
    CHECK(graticule.lines.size() == 2);
    CHECK(graticule.circles.size() == 2);
    CHECK(graticule.labels.size() == 4);
}

}  // namespace sidescopes
