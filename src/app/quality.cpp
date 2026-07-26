#include "app/quality.h"

#include "core/scopes/neutral.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"

namespace sidescopes {
namespace {

// Standard is the shipped behaviour, stated here rather than derived, so a
// change to it is a change to this table and nothing else.
constexpr QualityProfile Standard{
    .captureFramesPerSecond = 15,
    .magnificationTolerance = 1.4f,
    .columnTolerance = 1.4f,
    .waveformHeightCeiling = 512,
    .histogramCeiling = {4096, 768},
    .vectorscopeCeiling = 512,
    .neutralCeiling = MaximumNeutralSize,
    .sampleThinning = 1,
    .coarsenWhileDragged = true,
};

// Low takes the cheap precision first, in the order measurement ranked it: the
// vectorscope's image and the histogram's plot are 45-49% of their passes for
// deviations of a tenth and a hundredth of a code, the neutral plane is the
// largest memory item on the list with its cast reading unmoved, and the
// waveform gives up its height and the density of its columns but never the
// columns themselves.
constexpr QualityProfile Low{
    .captureFramesPerSecond = 10,
    .magnificationTolerance = 2.0f,
    .columnTolerance = Standard.columnTolerance,
    .waveformHeightCeiling = WaveformLevels,
    .histogramCeiling = {2048, 384},
    .vectorscopeCeiling = DefaultVectorscopeSize,
    .neutralCeiling = DefaultNeutralSize,
    .sampleThinning = 2,
    .coarsenWhileDragged = true,
};

// High spends on the two things it can still buy: images that are never
// magnified, and passes as often as the interface can show them. It buys
// nothing on the three axes measurement has closed - the vectorscope's image,
// the waveform's height, and a redraw rate past twenty a second.
constexpr QualityProfile High{
    .captureFramesPerSecond = 20,
    .magnificationTolerance = 1.0f,
    .columnTolerance = 1.0f,
    .waveformHeightCeiling = Standard.waveformHeightCeiling,
    .histogramCeiling = Standard.histogramCeiling,
    .vectorscopeCeiling = Standard.vectorscopeCeiling,
    .neutralCeiling = Standard.neutralCeiling,
    .sampleThinning = 1,
    .coarsenWhileDragged = false,
};

}  // namespace

const QualityProfile& profileFor(QualityLevel level)
{
    switch (level) {
    case QualityLevel::Low:
        return Low;
    case QualityLevel::High:
        return High;
    case QualityLevel::Standard:
        break;
    }

    return Standard;
}

std::string_view qualityToken(QualityLevel level)
{
    switch (level) {
    case QualityLevel::Low:
        return "low";
    case QualityLevel::High:
        return "high";
    case QualityLevel::Standard:
        break;
    }

    return "standard";
}

const char* qualityLabel(QualityLevel level)
{
    switch (level) {
    case QualityLevel::Low:
        return "Low";
    case QualityLevel::High:
        return "High";
    case QualityLevel::Standard:
        break;
    }

    return "Standard";
}

QualityLevel qualityFromToken(std::string_view token)
{
    for (const QualityLevel level : QualityLevels) {
        if (token == qualityToken(level)) {
            return level;
        }
    }

    return QualityLevel::Standard;
}

}  // namespace sidescopes
