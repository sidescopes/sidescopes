#include "core/color_lab.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sidescopes {
namespace {

// D65, as the sRGB primary matrix below actually realises it: the matrix rows
// sum to these values, so a full-scale white lands on L* 100 with a* and b*
// at zero by construction rather than by luck.
constexpr double WhiteX = 0.95047;
constexpr double WhiteY = 1.0;
constexpr double WhiteZ = 1.08883;

// Breakpoint and slope of the CIE f(t) curve: a straight segment replaces the
// cube root below Epsilon, where the root's slope runs away towards infinity.
constexpr double Epsilon = 216.0 / 24389.0;
constexpr double Kappa = 24389.0 / 27.0;

// 25^7, the pivot of every CIEDE2000 chroma rolloff.
constexpr double Pow25To7 = 6103515625.0;

constexpr double DegreesPerHalfTurn = 180.0;
constexpr double DegreesPerTurn = 360.0;

// Two hues exactly half a turn apart are a tie: the arcs either way round are
// the same length, and CIEDE2000 settles it by convention, taking the arc that
// does not cross the 0/360 seam. A separation only ever reaches that tie
// through two atan2 calls and a rollover past 360, which leave it good to
// about 1e-13 of a degree, so testing it against an exact half turn tests the
// noise. This tolerance sits orders of magnitude above that noise and orders
// below any separation that could carry meaning: every value from 1e-12 to
// 1e-3 reproduces the published test data identically.
constexpr double HueTieTolerance = 1e-9;

double toRadians(double degrees)
{
    return degrees * std::numbers::pi / DegreesPerHalfTurn;
}

double pow7(double value)
{
    const double squared = value * value;
    const double fourth = squared * squared;

    return fourth * squared * value;
}

double linearFromEncoded(double encoded)
{
    // The sRGB transfer function proper. The 2.2 power often quoted in its
    // place is a fit to the whole curve and drifts furthest exactly where
    // this linear toe lives, in the shadows.
    if (encoded <= 0.04045) {
        return encoded / 12.92;
    }

    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

double labCurve(double t)
{
    if (t > Epsilon) {
        return std::cbrt(t);
    }

    return (Kappa * t + 16.0) / 116.0;
}

// Hue angle in [0, 360). A colour on the neutral axis has no hue; atan2
// already answers zero there, and callers gate on chroma before leaning on
// the angle at all.
double hueAngleDegrees(double a, double b)
{
    if (a == 0.0 && b == 0.0) {
        return 0.0;
    }

    const double degrees = std::atan2(b, a) * DegreesPerHalfTurn / std::numbers::pi;

    return degrees < 0.0 ? degrees + DegreesPerTurn : degrees;
}

// The reported hue difference, wrapped to (-180, 180]. Half a turn is a tie
// here too -- the hue turned as far one way as the other -- but the chord it
// feeds has the same magnitude whichever way it resolves, so settling it on
// the positive half turn costs nothing.
double wrapHueDifference(double degrees)
{
    if (degrees > DegreesPerHalfTurn) {
        return degrees - DegreesPerTurn;
    }

    if (degrees <= -DegreesPerHalfTurn) {
        return degrees + DegreesPerTurn;
    }

    return degrees;
}

}  // namespace

LabColor labFromSrgb(const FloatColor& srgb)
{
    const double red = linearFromEncoded(std::clamp(srgb.r / 255.0, 0.0, 1.0));
    const double green = linearFromEncoded(std::clamp(srgb.g / 255.0, 0.0, 1.0));
    const double blue = linearFromEncoded(std::clamp(srgb.b / 255.0, 0.0, 1.0));

    // sRGB primaries under D65.
    const double x = 0.4124564 * red + 0.3575761 * green + 0.1804375 * blue;
    const double y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue;
    const double z = 0.0193339 * red + 0.1191920 * green + 0.9503041 * blue;

    const double fx = labCurve(x / WhiteX);
    const double fy = labCurve(y / WhiteY);
    const double fz = labCurve(z / WhiteZ);

    LabColor lab;
    lab.lightness = static_cast<float>(116.0 * fy - 16.0);
    lab.a = static_cast<float>(500.0 * (fx - fy));
    lab.b = static_cast<float>(200.0 * (fy - fz));

    return lab;
}

float chromaOf(const LabColor& lab)
{
    return std::hypot(lab.a, lab.b);
}

ColorDifference differenceFrom(const LabColor& reference, const LabColor& sample)
{
    const float referenceChroma = chromaOf(reference);
    const float sampleChroma = chromaOf(sample);
    const double hueDifference =
        wrapHueDifference(hueAngleDegrees(sample.a, sample.b) - hueAngleDegrees(reference.a, reference.b));

    ColorDifference difference;
    difference.lightness = sample.lightness - reference.lightness;
    difference.chroma = sampleChroma - referenceChroma;
    // dH* is the chord across the hue circle weighted by sqrt(C1*C2), not the
    // bare angle between the two hues. An angle is meaningless near the
    // neutral axis, where two greys a rounding error apart can sit half a
    // turn from each other; the chroma weight sends the chord to zero exactly
    // as either colour approaches neutral, so the term gates itself and needs
    // no chroma threshold of its own. The sign survives the weighting and
    // still says which way the hue turned.
    difference.hue = static_cast<float>(2.0 * std::sqrt(static_cast<double>(referenceChroma) * sampleChroma) *
                                        std::sin(toRadians(hueDifference) / 2.0));
    difference.deltaE = deltaE2000(reference, sample);

    return difference;
}

float deltaE2000(const LabColor& first, const LabColor& second)
{
    const double lightness1 = first.lightness;
    const double lightness2 = second.lightness;
    const double a1 = first.a;
    const double a2 = second.a;
    const double b1 = first.b;
    const double b2 = second.b;

    // The G correction stretches a* in the low-chroma region, where CIELAB
    // spaces neutrals too tightly. It keys off the mean of the two original
    // chromas, not the corrected ones.
    const double meanChroma = (std::hypot(a1, b1) + std::hypot(a2, b2)) / 2.0;
    const double meanChroma7 = pow7(meanChroma);
    const double g = 0.5 * (1.0 - std::sqrt(meanChroma7 / (meanChroma7 + Pow25To7)));

    const double aPrime1 = (1.0 + g) * a1;
    const double aPrime2 = (1.0 + g) * a2;
    const double chromaPrime1 = std::hypot(aPrime1, b1);
    const double chromaPrime2 = std::hypot(aPrime2, b2);
    const double chromaPrimeProduct = chromaPrime1 * chromaPrime2;
    const double huePrime1 = hueAngleDegrees(aPrime1, b1);
    const double huePrime2 = hueAngleDegrees(aPrime2, b2);

    // The hue difference and the mean hue turn on one question: do the two
    // hues sit more than half a turn apart as plain numbers, so that the arc
    // between them crosses the 0/360 seam? The formula keys both rules off
    // that single fact, and deriving both from one answer stops them ever
    // disagreeing about it.
    const double hueSpread = huePrime2 - huePrime1;
    const bool crossesSeam = std::abs(hueSpread) > DegreesPerHalfTurn + HueTieTolerance;

    // Either chroma at zero leaves the hue angle indeterminate, so the hue
    // difference is defined as zero and the mean hue as the sum, which is the
    // surviving colour's own hue. Reading the angle atan2 happens to return
    // there instead is the classic way to get a wrong dE against a neutral.
    const bool bothHaveChroma = chromaPrimeProduct != 0.0;
    double deltaHuePrime = 0.0;
    if (bothHaveChroma) {
        deltaHuePrime = hueSpread;
        if (crossesSeam) {
            deltaHuePrime += hueSpread > 0.0 ? -DegreesPerTurn : DegreesPerTurn;
        }
    }

    const double deltaCapitalHPrime = 2.0 * std::sqrt(chromaPrimeProduct) * std::sin(toRadians(deltaHuePrime) / 2.0);

    const double hueSum = huePrime1 + huePrime2;
    double meanHuePrime = hueSum;
    if (bothHaveChroma) {
        if (!crossesSeam) {
            meanHuePrime = hueSum / 2.0;
        } else if (hueSum < DegreesPerTurn) {
            meanHuePrime = (hueSum + DegreesPerTurn) / 2.0;
        } else {
            meanHuePrime = (hueSum - DegreesPerTurn) / 2.0;
        }
    }

    const double meanLightness = (lightness1 + lightness2) / 2.0;
    const double meanChromaPrime = (chromaPrime1 + chromaPrime2) / 2.0;

    const double t =
        1.0 - 0.17 * std::cos(toRadians(meanHuePrime - 30.0)) + 0.24 * std::cos(toRadians(2.0 * meanHuePrime)) +
        0.32 * std::cos(toRadians(3.0 * meanHuePrime + 6.0)) - 0.20 * std::cos(toRadians(4.0 * meanHuePrime - 63.0));

    const double lightnessOffset = meanLightness - 50.0;
    const double lightnessWeight =
        1.0 + (0.015 * lightnessOffset * lightnessOffset) / std::sqrt(20.0 + lightnessOffset * lightnessOffset);
    const double chromaWeight = 1.0 + 0.045 * meanChromaPrime;
    const double hueWeight = 1.0 + 0.015 * meanChromaPrime * t;

    // RT rotates the chroma/hue error ellipse through the blue region, where
    // the constant-hue loci bend hardest.
    const double meanChromaPrime7 = pow7(meanChromaPrime);
    const double rotationOffset = (meanHuePrime - 275.0) / 25.0;
    const double rotationAngle = 30.0 * std::exp(-rotationOffset * rotationOffset);
    const double chromaRolloff = 2.0 * std::sqrt(meanChromaPrime7 / (meanChromaPrime7 + Pow25To7));
    const double rotationTerm = -std::sin(toRadians(2.0 * rotationAngle)) * chromaRolloff;

    // Every parametric factor is 1, so the weights divide the errors directly.
    const double lightnessError = (lightness2 - lightness1) / lightnessWeight;
    const double chromaError = (chromaPrime2 - chromaPrime1) / chromaWeight;
    const double hueError = deltaCapitalHPrime / hueWeight;

    return static_cast<float>(std::sqrt(lightnessError * lightnessError + chromaError * chromaError +
                                        hueError * hueError + rotationTerm * chromaError * hueError));
}

}  // namespace sidescopes
