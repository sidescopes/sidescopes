#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/frame.h"
#include "core/scopes/histogram.h"
#include "core/scopes/neutral.h"
#include "core/scopes/sampling.h"
#include "core/scopes/vectorscope.h"
#include "core/scopes/waveform.h"

namespace sidescopes {
namespace {

// The regions the policy has to get right: a swatch, a photo-editor canvas,
// a whole 1080p screen, a whole Retina laptop display, and a 5K panel.
constexpr IntRect Swatch{0, 0, 200, 200};
constexpr IntRect Canvas{0, 0, 1000, 700};
constexpr IntRect Screen1080{0, 0, 1920, 1080};
constexpr IntRect Retina{0, 0, 3456, 2234};
constexpr IntRect Panel5K{0, 0, 5120, 2880};

long long samplesIn(const SampleGrid& grid)
{
    return static_cast<long long>(grid.rows) * grid.columnsPerRow;
}

}  // namespace

TEST_CASE("Everyday regions are sampled whole")
{
    // Nothing a photographer scopes day to day reaches the budget, so the
    // thinning must be invisible there - not merely small.
    for (const IntRect& region : {Swatch, Canvas, Screen1080}) {
        CAPTURE(region.width, region.height);
        const SampleGrid grid = sampleGridFor(1, region, SampleBudget);
        CHECK(grid.rowStride == 1);
        CHECK(grid.columnStride == 1);
        CHECK(grid.rows == region.height);
        CHECK(grid.columnsPerRow == region.width);
    }
}

TEST_CASE("A region past the budget is thinned by rows until it fits")
{
    const SampleGrid retina = sampleGridFor(1, Retina, SampleBudget);
    CHECK(retina.rowStride == 2);
    CHECK(retina.columnStride == 1);
    CHECK(samplesIn(retina) <= SampleBudget);

    // A bigger panel thins further rather than giving up at two.
    const SampleGrid panel = sampleGridFor(1, Panel5K, SampleBudget);
    CHECK(panel.rowStride == 4);
    CHECK(samplesIn(panel) <= SampleBudget);

    // Columns are never dropped: an image column that some source columns
    // never reach is the density comb the fractional splat exists to prevent.
    CHECK(panel.columnStride == 1);
    CHECK(panel.columnsPerRow == Panel5K.width);
}

TEST_CASE("The user's stride is a floor the policy never undercuts")
{
    // Asking for coarser sampling must still give coarser sampling, on both
    // axes, whatever the budget would have chosen.
    const SampleGrid coarse = sampleGridFor(4, Canvas, SampleBudget);
    CHECK(coarse.rowStride == 4);
    CHECK(coarse.columnStride == 4);
    CHECK(coarse.rows == 175);
    CHECK(coarse.columnsPerRow == 250);

    // Out-of-range values clamp into the parameter's own 1..8 range.
    CHECK(sampleGridFor(0, Canvas, SampleBudget).columnStride == 1);
    CHECK(sampleGridFor(99, Canvas, SampleBudget).columnStride == 8);

    // A user stride that already fits is not raised further.
    const SampleGrid strided = sampleGridFor(2, Retina, SampleBudget);
    CHECK(strided.rowStride == 2);
    CHECK(strided.columnStride == 2);
}

TEST_CASE("An unlimited budget samples every row however large the region")
{
    // What the waveform asks for: its bins are too sparsely populated to
    // afford thinning, so the policy must leave it alone at any size.
    for (const IntRect& region : {Retina, Panel5K}) {
        CAPTURE(region.width, region.height);
        const SampleGrid grid = sampleGridFor(1, region, UnlimitedSamples);
        CHECK(grid.rowStride == 1);
        CHECK(grid.rows == region.height);
    }

    // The user's own stride still applies.
    CHECK(sampleGridFor(3, Retina, UnlimitedSamples).rowStride == 3);
}

TEST_CASE("The row phase walks so sampling cannot resonate with the content")
{
    // This is the whole accuracy argument for thinning. A fixed phase samples
    // one parity of a row-banded image and never the other, and screen content
    // is full of such patterns; measured on content banded at every second and
    // third row, a fixed phase left the vectorscope a mean of 7.98 of 255 from
    // the unthinned image against 0.15 staggered.
    for (const int stride : {2, 3, 4, 5, 8}) {
        CAPTURE(stride);
        const IntRect region{0, 0, 64, 4096};
        const SampleGrid grid = sampleGridFor(stride, region, UnlimitedSamples);
        REQUIRE(grid.rowStride == stride);

        std::vector<int> residueCounts(static_cast<std::size_t>(stride), 0);
        int previous = -1;
        for (int index = 0; index < grid.rows; ++index) {
            const int row = sampleRowOf(grid, region, index);
            // Always inside the region, and always forward.
            REQUIRE(row >= region.y);
            REQUIRE(row < region.y + region.height);
            REQUIRE(row > previous);
            previous = row;
            ++residueCounts[static_cast<std::size_t>(row % stride)];
        }

        // Every residue class is visited, and none dominates: a stride of two
        // must not see only even rows. The one imbalance allowed is the tail -
        // the last indices whose phase would leave the region fall back to the
        // unstaggered row, which always lands on residue zero.
        const int fewest = *std::min_element(residueCounts.begin(), residueCounts.end());
        const int most = *std::max_element(residueCounts.begin(), residueCounts.end());
        const int evenShare = grid.rows / stride;
        CHECK(fewest > 0);
        CHECK(fewest >= evenShare - 1);
        CHECK(most <= evenShare + 1);
    }
}

TEST_CASE("A phase that would leave the region falls back inside it")
{
    // The last indices of a run can carry the phase past the final row; those
    // must land on the unstaggered row rather than off the end.
    for (int height = 1; height < 40; ++height) {
        CAPTURE(height);
        const IntRect region{0, 7, 16, height};
        for (const int stride : {1, 2, 3, 5, 8}) {
            const SampleGrid grid = sampleGridFor(stride, region, UnlimitedSamples);
            for (int index = 0; index < grid.rows; ++index) {
                const int row = sampleRowOf(grid, region, index);
                REQUIRE(row >= region.y);
                REQUIRE(row < region.y + region.height);
            }
        }
    }
}

TEST_CASE("The budgets are the ones the measurements were taken at")
{
    // Every number here was chosen from measured accuracy, recorded in
    // notes/perf-findings.md, so moving any of them is a decision rather than a
    // tweak. The ceiling sits just above half a 4K frame, which is what keeps
    // every display up to 1440p sampled row for row.
    CHECK(SampleBudget == 4'200'000);
    CHECK(SampleBudget > static_cast<long long>(1920) * 1080);
    CHECK(SampleBudget > static_cast<long long>(2560) * 1440);
    CHECK(SampleBudget < static_cast<long long>(3456) * 2234);
    CHECK(UnlimitedSamples == 0);

    // Per bin, calibrated per scope over a whole 3456x2234 display: the
    // histogram's bins are fixed and were holding ten thousand samples each; the
    // vectorscope's density estimate suppresses bin noise the waveform's
    // log-and-gamma amplifies, so the waveform's minimum is the highest here.
    CHECK(HistogramMinSamplesPerBin == 1000);
    CHECK(VectorscopeMinSamplesPerBin == 24);
    CHECK(NeutralMinSamplesPerBin == 24);
    CHECK(WaveformMinSamplesPerBin == 32);
}

TEST_CASE("A scope with fixed bins stops costing more as the region grows")
{
    // The histogram's precision cannot improve past its 768 bins however large
    // the region is, so its budget does not grow with one. This is what makes it
    // cheap behind a full-screen region, where it was taking every second row of
    // a 7.7 million pixel frame to fill bins that were already saturated.
    constexpr long long HistogramBins = 256 * 3;
    const long long budget = budgetForBins(HistogramBins, HistogramMinSamplesPerBin);
    CHECK(budget == 768'000);

    const SampleGrid display = sampleGridFor(1, IntRect{0, 0, 3456, 2234}, budget);
    CHECK(display.rowStride > 1);
    // Still a thousand samples a bin at the largest region a display offers,
    // which is the whole argument for thinning it this far.
    CHECK(static_cast<long long>(display.rows) * display.columnsPerRow / HistogramBins > 1000);

    // A region small enough to fit the budget is sampled whole.
    const SampleGrid canvas = sampleGridFor(1, IntRect{0, 0, 900, 700}, budget);
    CHECK(canvas.rowStride == 1);
}

TEST_CASE("A scope whose bins follow its pane thins only while it is small")
{
    // The waveform's bins are columns x levels, so its budget grows with the
    // pane it is drawn into. That is the owner's principle in one line: a small
    // waveform can afford to thin, a large one cannot, and one global budget
    // could not tell them apart.
    const IntRect display{0, 0, 3456, 2234};
    const auto budgetAt = [](int columns) {
        return budgetForBins(static_cast<long long>(columns) * WaveformLevels, WaveformMinSamplesPerBin);
    };

    CHECK(sampleGridFor(1, display, budgetAt(512)).rowStride > 1);
    // From the default width up, every row - including at every width above it.
    for (const int columns : {DefaultWaveformColumns, 2048, MaximumWaveformColumns}) {
        CHECK(sampleGridFor(1, display, budgetAt(columns)).rowStride == 1);
    }
}

TEST_CASE("The neutral plane's budget is capped however many bins it grows")
{
    // Alone among the scopes the neutral one converts every sample to L*a*b*, so
    // an unbounded pass over a whole display measured 69 ms against 14 at the
    // ceiling. Its plane follows its pane up to a million bins, which would ask
    // for twenty-five million samples; the ceiling is what keeps a large plane
    // costing what it costs today rather than five times that.
    const long long uncapped = budgetForBins(1024LL * 1024, NeutralMinSamplesPerBin);
    CHECK(uncapped > SampleBudget);
    CHECK(std::min(uncapped, SampleBudget) == SampleBudget);

    // At the default plane the derived budget is what applies, and it is well
    // below the ceiling - which is where the saving comes from.
    const long long derived = budgetForBins(256LL * 256, NeutralMinSamplesPerBin);
    CHECK(derived < SampleBudget);
}

TEST_CASE("An empty region asks for no samples")
{
    const SampleGrid grid = sampleGridFor(1, IntRect{10, 10, 0, 0}, SampleBudget);
    CHECK(grid.rows == 0);
    CHECK(grid.columnsPerRow == 0);
}

}  // namespace sidescopes
