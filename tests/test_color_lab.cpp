#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "core/color_lab.h"
#include "core/frame.h"

namespace sidescopes {
namespace {

struct SharmaPair
{
    LabColor first;
    LabColor second;
    double expected;
};

// The 34 published pairs from G. Sharma, W. Wu and E. N. Dalal, "The CIEDE2000
// Color-Difference Formula: Implementation Notes, Supplementary Test Data, and
// Mathematical Observations", Color Research and Application 30(1), 21-30,
// 2005, transcribed from the authors' dataset:
// https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/dataNprograms/ciede2000testdata.txt
// The set exists to catch the mistakes the formula invites, and a plausible
// implementation passes most of it while failing a handful: pairs 1-6 probe
// the blue region the RT rotation term exists for, 7-8 a colour against a
// neutral whose hue angle is indeterminate, and 9-15 the hue wrap, where two
// colours a hair either side of half a turn apart must not collapse onto the
// same answer. Values are the published ones, to four decimals.
constexpr SharmaPair SharmaPairs[] = {
    {LabColor{50.0000f, 2.6772f, -79.7751f}, LabColor{50.0000f, 0.0000f, -82.7485f}, 2.0425},
    {LabColor{50.0000f, 3.1571f, -77.2803f}, LabColor{50.0000f, 0.0000f, -82.7485f}, 2.8615},
    {LabColor{50.0000f, 2.8361f, -74.0200f}, LabColor{50.0000f, 0.0000f, -82.7485f}, 3.4412},
    {LabColor{50.0000f, -1.3802f, -84.2814f}, LabColor{50.0000f, 0.0000f, -82.7485f}, 1.0000},
    {LabColor{50.0000f, -1.1848f, -84.8006f}, LabColor{50.0000f, 0.0000f, -82.7485f}, 1.0000},
    {LabColor{50.0000f, -0.9009f, -85.5211f}, LabColor{50.0000f, 0.0000f, -82.7485f}, 1.0000},
    {LabColor{50.0000f, 0.0000f, 0.0000f}, LabColor{50.0000f, -1.0000f, 2.0000f}, 2.3669},
    {LabColor{50.0000f, -1.0000f, 2.0000f}, LabColor{50.0000f, 0.0000f, 0.0000f}, 2.3669},
    {LabColor{50.0000f, 2.4900f, -0.0010f}, LabColor{50.0000f, -2.4900f, 0.0009f}, 7.1792},
    {LabColor{50.0000f, 2.4900f, -0.0010f}, LabColor{50.0000f, -2.4900f, 0.0010f}, 7.1792},
    {LabColor{50.0000f, 2.4900f, -0.0010f}, LabColor{50.0000f, -2.4900f, 0.0011f}, 7.2195},
    {LabColor{50.0000f, 2.4900f, -0.0010f}, LabColor{50.0000f, -2.4900f, 0.0012f}, 7.2195},
    {LabColor{50.0000f, -0.0010f, 2.4900f}, LabColor{50.0000f, 0.0009f, -2.4900f}, 4.8045},
    {LabColor{50.0000f, -0.0010f, 2.4900f}, LabColor{50.0000f, 0.0010f, -2.4900f}, 4.8045},
    {LabColor{50.0000f, -0.0010f, 2.4900f}, LabColor{50.0000f, 0.0011f, -2.4900f}, 4.7461},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{50.0000f, 0.0000f, -2.5000f}, 4.3065},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{73.0000f, 25.0000f, -18.0000f}, 27.1492},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{61.0000f, -5.0000f, 29.0000f}, 22.8977},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{56.0000f, -27.0000f, -3.0000f}, 31.9030},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{58.0000f, 24.0000f, 15.0000f}, 19.4535},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{50.0000f, 3.1736f, 0.5854f}, 1.0000},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{50.0000f, 3.2972f, 0.0000f}, 1.0000},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{50.0000f, 1.8634f, 0.5757f}, 1.0000},
    {LabColor{50.0000f, 2.5000f, 0.0000f}, LabColor{50.0000f, 3.2592f, 0.3350f}, 1.0000},
    {LabColor{60.2574f, -34.0099f, 36.2677f}, LabColor{60.4626f, -34.1751f, 39.4387f}, 1.2644},
    {LabColor{63.0109f, -31.0961f, -5.8663f}, LabColor{62.8187f, -29.7946f, -4.0864f}, 1.2630},
    {LabColor{61.2901f, 3.7196f, -5.3901f}, LabColor{61.4292f, 2.2480f, -4.9620f}, 1.8731},
    {LabColor{35.0831f, -44.1164f, 3.7933f}, LabColor{35.0232f, -40.0716f, 1.5901f}, 1.8645},
    {LabColor{22.7233f, 20.0904f, -46.6940f}, LabColor{23.0331f, 14.9730f, -42.5619f}, 2.0373},
    {LabColor{36.4612f, 47.8580f, 18.3852f}, LabColor{36.2715f, 50.5065f, 21.2231f}, 1.4146},
    {LabColor{90.8027f, -2.0831f, 1.4410f}, LabColor{91.1528f, -1.6435f, 0.0447f}, 1.4441},
    {LabColor{90.9257f, -0.5406f, -0.9208f}, LabColor{88.6381f, -0.8985f, -0.7239f}, 1.5381},
    {LabColor{6.7747f, -0.2908f, -2.4247f}, LabColor{5.8714f, -0.0985f, -2.2286f}, 0.6377},
    {LabColor{2.0776f, 0.0795f, -1.1350f}, LabColor{0.9033f, -0.0636f, -0.5514f}, 0.9082},
};

// Hue angle in [0, 360), computed here rather than reached for through the
// header: the self-gating test has to describe its colours in the very terms
// dH* refuses to use.
double hueAngleDegrees(const LabColor& lab)
{
    const double degrees =
        std::atan2(static_cast<double>(lab.b), static_cast<double>(lab.a)) * 180.0 / std::numbers::pi;

    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

}  // namespace

TEST_CASE("CIEDE2000 matches the published Sharma test data")
{
    for (const SharmaPair& pair : SharmaPairs) {
        CAPTURE(pair.first.lightness, pair.first.a, pair.first.b);
        CAPTURE(pair.second.lightness, pair.second.a, pair.second.b);
        CHECK(deltaE2000(pair.first, pair.second) == Catch::Approx(pair.expected).margin(1e-4));
    }
}

TEST_CASE("CIEDE2000 vanishes on a colour against itself and reads the same either way round")
{
    for (const SharmaPair& pair : SharmaPairs) {
        CAPTURE(pair.first.lightness, pair.first.a, pair.first.b);
        CAPTURE(pair.second.lightness, pair.second.a, pair.second.b);
        CHECK(deltaE2000(pair.first, pair.first) == 0.0f);
        CHECK(deltaE2000(pair.second, pair.second) == 0.0f);
        // Symmetry is not free: dC and dH both flip sign when the arguments
        // swap, and only their product enters the RT term, so a hue wrap that
        // is not antisymmetric shows up here as two distances for one pair.
        CHECK(deltaE2000(pair.first, pair.second) == deltaE2000(pair.second, pair.first));
    }
}

TEST_CASE("Hues exactly half a turn apart resolve to the near arc")
{
    // Pairs 9-12 of the published data walk b* across the point where the two
    // hues are exactly half a turn apart, and pair 10 lands on it: the arcs
    // either way round are the same length, and the answer swings by 0.04
    // depending on which one the mean hue is taken across. The pair is
    // antipodal to the last bit -- a2 is exactly -a1 and b2 exactly -b1 -- so
    // the separation is exactly half a turn however the inputs round, and the
    // convention settles it on the arc that does not cross the 0/360 seam.
    // Reached by way of two atan2 calls the separation computes as
    // 180.00000000000003, and a bare > 180 test reads that noise as a real
    // crossing and takes the far arc.
    const LabColor onTheSeam{50.0000f, 2.4900f, -0.0010f};
    const LabColor antipodal{50.0000f, -2.4900f, 0.0010f};
    CHECK(deltaE2000(onTheSeam, antipodal) == Catch::Approx(7.1792).margin(1e-4));

    // A hair either side of the tie the crossing is real, and the published
    // data records the step it causes.
    CHECK(deltaE2000(onTheSeam, LabColor{50.0000f, -2.4900f, 0.0009f}) == Catch::Approx(7.1792).margin(1e-4));
    CHECK(deltaE2000(onTheSeam, LabColor{50.0000f, -2.4900f, 0.0011f}) == Catch::Approx(7.2195).margin(1e-4));
}

TEST_CASE("sRGB anchors land on their CIELAB values")
{
    const LabColor white = labFromSrgb(FloatColor{255.0f, 255.0f, 255.0f});
    CHECK(white.lightness == Catch::Approx(100.0).margin(1e-3));
    CHECK(white.a == Catch::Approx(0.0).margin(1e-3));
    CHECK(white.b == Catch::Approx(0.0).margin(1e-3));

    const LabColor black = labFromSrgb(FloatColor{0.0f, 0.0f, 0.0f});
    CHECK(black.lightness == Catch::Approx(0.0).margin(1e-6));
    CHECK(chromaOf(black) == Catch::Approx(0.0).margin(1e-6));

    // Mid grey sits at 53.6, not 50: L* tracks perceived lightness, and the
    // sRGB transfer function has already bent the code value on its way to the
    // linear intensity this reads.
    const LabColor midGrey = labFromSrgb(FloatColor{128.0f, 128.0f, 128.0f});
    CHECK(midGrey.lightness == Catch::Approx(53.585).margin(1e-3));
    CHECK(chromaOf(midGrey) == Catch::Approx(0.0).margin(1e-3));
}

TEST_CASE("Neutral greys carry no chroma at any level")
{
    float previousLightness = -1.0f;
    for (const float level : {0.0f, 16.0f, 32.0f, 64.0f, 128.0f, 192.0f, 255.0f}) {
        const LabColor grey = labFromSrgb(FloatColor{level, level, level});
        CAPTURE(level);
        CHECK(chromaOf(grey) == Catch::Approx(0.0).margin(1e-3));
        CHECK(grey.lightness > previousLightness);
        previousLightness = grey.lightness;
    }
}

TEST_CASE("Chroma is the distance from the neutral axis")
{
    CHECK(chromaOf(LabColor{50.0f, 3.0f, 4.0f}) == Catch::Approx(5.0));
    CHECK(chromaOf(LabColor{50.0f, -3.0f, -4.0f}) == Catch::Approx(5.0));
    CHECK(chromaOf(LabColor{50.0f, 0.0f, 0.0f}) == 0.0f);
}

TEST_CASE("Hue difference gates itself as the colours approach neutral")
{
    // Two pairs at the same two hue angles, differing only in how far they sit
    // from the neutral axis. A raw angle difference cannot tell them apart:
    // it reports the same near-maximal hue error for both.
    const LabColor nearNeutralWarm{50.0f, 0.02f, 0.01f};
    const LabColor nearNeutralCool{50.0f, -0.015f, -0.005f};
    const LabColor saturatedWarm{50.0f, 40.0f, 20.0f};
    const LabColor saturatedCool{50.0f, -30.0f, -10.0f};

    // The comparison only means anything if the angles really do line up.
    CHECK(hueAngleDegrees(nearNeutralWarm) == Catch::Approx(hueAngleDegrees(saturatedWarm)));
    CHECK(hueAngleDegrees(nearNeutralCool) == Catch::Approx(hueAngleDegrees(saturatedCool)));
    CHECK(hueAngleDegrees(nearNeutralCool) - hueAngleDegrees(nearNeutralWarm) == Catch::Approx(171.87).margin(0.01));

    const ColorDifference nearNeutral = differenceFrom(nearNeutralWarm, nearNeutralCool);
    const ColorDifference saturated = differenceFrom(saturatedWarm, saturatedCool);

    // Most of a turn of hue between two all-but-greys is noise, and dH* says
    // so without being told where neutral starts.
    CHECK(std::abs(nearNeutral.hue) < 0.05f);
    // The same turn between two saturated colours is a real difference.
    CHECK(std::abs(saturated.hue) > 70.0f);

    // On the axis itself the chord is exactly zero, no threshold in sight.
    CHECK(differenceFrom(LabColor{50.0f, 0.0f, 0.0f}, LabColor{60.0f, 0.0f, 0.0f}).hue == 0.0f);
}

TEST_CASE("Hue difference keeps the direction the hue turned")
{
    const LabColor reference{50.0f, 30.0f, 0.0f};

    // Turning towards +b* (yellow) and towards -b* (blue) must not read alike.
    CHECK(differenceFrom(reference, LabColor{50.0f, 30.0f, 20.0f}).hue > 0.0f);
    CHECK(differenceFrom(reference, LabColor{50.0f, 30.0f, -20.0f}).hue < 0.0f);
}

TEST_CASE("Lightness and chroma differences carry the direction of the change")
{
    const LabColor reference{50.0f, 20.0f, 10.0f};

    // A brighter sample reads positive on dL*, a darker one negative.
    CHECK(differenceFrom(reference, LabColor{60.0f, 20.0f, 10.0f}).lightness > 0.0f);
    CHECK(differenceFrom(reference, LabColor{40.0f, 20.0f, 10.0f}).lightness < 0.0f);

    // Scaling a* and b* together holds the hue angle and moves only chroma, so
    // a more saturated sample reads positive on dC* and nothing else.
    const ColorDifference moreSaturated = differenceFrom(reference, LabColor{50.0f, 40.0f, 20.0f});
    CHECK(moreSaturated.chroma > 0.0f);
    CHECK(moreSaturated.lightness == 0.0f);
    CHECK(moreSaturated.hue == Catch::Approx(0.0).margin(1e-4));

    CHECK(differenceFrom(reference, LabColor{50.0f, 10.0f, 5.0f}).chroma < 0.0f);
}

TEST_CASE("The reported difference carries the CIEDE2000 magnitude")
{
    for (const SharmaPair& pair : SharmaPairs) {
        CAPTURE(pair.first.lightness, pair.first.a, pair.first.b);
        CHECK(differenceFrom(pair.first, pair.second).deltaE == deltaE2000(pair.first, pair.second));
    }
}

}  // namespace sidescopes
