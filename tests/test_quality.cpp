#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <utility>

#include "app/frame_pacing.h"
#include "app/quality.h"
#include "core/scopes/neutral.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"

namespace sidescopes {

TEST_CASE("Standard is the shipped behaviour")
{
    // Every other level is a deliberate departure from this one, and the exact
    // scope goldens are what prove the departure is only where it is meant to
    // be. Pinned so that changing Standard is a change to this table.
    const QualityProfile& standard = profileFor(QualityLevel::Standard);
    CHECK(standard.captureFramesPerSecond == 15);
    CHECK(standard.magnificationTolerance == 1.4f);
    CHECK(standard.columnTolerance == 1.4f);
    CHECK(standard.waveformHeightCeiling == 512);
    CHECK(standard.histogramCeiling == std::pair<int, int>{4096, 768});
    CHECK(standard.vectorscopeCeiling == 512);
    CHECK(standard.neutralCeiling == MaximumNeutralSize);
    CHECK(standard.sampleThinning == 1);
    CHECK(standard.coarsenWhileDragged);
}

TEST_CASE("Every level differs from the one below it")
{
    // Three levels earn their place only by being different. The pairs are
    // checked whole, so a level that stopped differing anywhere fails here
    // rather than quietly costing the same as its neighbour.
    const QualityProfile& low = profileFor(QualityLevel::Low);
    const QualityProfile& standard = profileFor(QualityLevel::Standard);
    const QualityProfile& high = profileFor(QualityLevel::High);

    CHECK_FALSE(low == standard);
    CHECK_FALSE(standard == high);
    CHECK(low.captureFramesPerSecond < standard.captureFramesPerSecond);
    CHECK(standard.captureFramesPerSecond < high.captureFramesPerSecond);
    CHECK(low.magnificationTolerance > standard.magnificationTolerance);
    CHECK(standard.magnificationTolerance > high.magnificationTolerance);
}

TEST_CASE("Low gives up resolution and never a place in the region")
{
    // The waveform's columns are the one axis that carries data rather than
    // resolution, so the level that gives up the most gives up none of them:
    // scanning for a blown highlight is exactly when that scope matters.
    const QualityProfile& low = profileFor(QualityLevel::Low);
    const QualityProfile& standard = profileFor(QualityLevel::Standard);

    CHECK(low.columnTolerance == standard.columnTolerance);
    CHECK(low.vectorscopeCeiling < standard.vectorscopeCeiling);
    CHECK(low.neutralCeiling < standard.neutralCeiling);
    CHECK(low.histogramCeiling.first < standard.histogramCeiling.first);
    CHECK(low.waveformHeightCeiling < standard.waveformHeightCeiling);
    CHECK(low.sampleThinning > standard.sampleThinning);
}

TEST_CASE("High buys nothing on the axes measurement has closed")
{
    // Three axes were measured as waste and must stay closed, whatever a
    // level is willing to spend. The vectorscope's image resolves nothing its
    // 256-code grid has not already spread; the waveform's height only draws a
    // finer spline through levels eight-bit input fixes at 256; and a pass
    // computed between two drawn frames is never shown at all.
    const QualityProfile& high = profileFor(QualityLevel::High);
    const QualityProfile& standard = profileFor(QualityLevel::Standard);

    CHECK(high.vectorscopeCeiling <= standard.vectorscopeCeiling);
    CHECK(high.waveformHeightCeiling <= standard.waveformHeightCeiling);
    CHECK(high.neutralCeiling <= MaximumNeutralSize);
    CHECK(high.histogramCeiling.second <= MaximumWaveformHeight);

    const double redrawsPerSecond = 1.0 / ContentRedrawSeconds;
    CHECK(static_cast<double>(high.captureFramesPerSecond) <= redrawsPerSecond);
}

TEST_CASE("No level tolerates more magnification on a column than Standard")
{
    // A column dropped is a column of the picture never looked at, and the
    // cost says so: halving them moves the trace by 5.5 to 10.6 of 255 against
    // 0.03 to 2.8 for every other axis.
    const QualityProfile& standard = profileFor(QualityLevel::Standard);
    for (const QualityLevel level : QualityLevels) {
        CHECK(profileFor(level).columnTolerance <= standard.columnTolerance);
        CHECK(profileFor(level).sampleThinning >= 1);
    }
}

TEST_CASE("A level survives the preferences file")
{
    for (const QualityLevel level : QualityLevels) {
        CHECK(qualityFromToken(qualityToken(level)) == level);
    }
    CHECK(qualityToken(QualityLevel::Standard) == "standard");
    CHECK(std::string_view{qualityLabel(QualityLevel::Standard)} == "Standard");

    // The file is hand-editable, so a word no level answers to is a missing
    // setting rather than an error.
    CHECK(qualityFromToken("") == QualityLevel::Standard);
    CHECK(qualityFromToken("ludicrous") == QualityLevel::Standard);
}

}  // namespace sidescopes
