#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <vector>

#include "core/scopes/histogram.h"
#include "core/scopes/histogram_bins.h"
#include "core/scopes/sampling.h"
#include "scope_image.h"
#include "test_frame.h"

namespace sidescopes {

using namespace test;

namespace {

// Plot height for a bin in one channel (0=r, 1=g, 2=b): the tallest of
// the image columns that render that bin.
int barHeight(const ScopeImage& image, int value, int channel)
{
    const int columnsPerBin = image.width / Histogram::Bins;
    int height = 0;
    for (int column = value * columnsPerBin; column < (value + 1) * columnsPerBin; ++column) {
        int columnHeight = 0;
        for (int row = 0; row < image.height; ++row) {
            if (image.rgba[(static_cast<std::size_t>(row) * image.width + column) * 4 + channel] > 0) {
                ++columnHeight;
            }
        }
        height = std::max(height, columnHeight);
    }
    return height;
}

// Bins with a nonzero plot in one channel.
std::vector<int> litValues(const ScopeImage& image, int channel)
{
    std::vector<int> values;
    for (int value = 0; value < Histogram::Bins; ++value) {
        if (barHeight(image, value, channel) > 0) {
            values.push_back(value);
        }
    }
    return values;
}

// A photograph-ish frame: the shared and unshared paths must agree over content
// with real structure, not only over a flat fill that every path draws
// identically.
TestFrame texturedFrame()
{
    TestFrame frame(200, 120, 0);
    for (int py = 0; py < 120; ++py) {
        for (int px = 0; px < 200; ++px) {
            const auto value = static_cast<uint8_t>((px * 7 + py * 13) % 256);
            frame.setColor(px, py,
                           Color{value, static_cast<uint8_t>(255 - value), static_cast<uint8_t>((value * 3) % 256)});
        }
    }

    return frame;
}

// The image an engine of its own draws in @p style, which is what a shared one
// has to reproduce byte for byte.
std::vector<uint8_t> unsharedImage(const TestFrame& frame, HistogramStyle style)
{
    HistogramSettings settings;
    settings.style = style;
    Histogram scope;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, frame.width, frame.height});

    return scope.image().rgba;
}

}  // namespace

TEST_CASE("Two histogram scopes over one frame scatter it once")
{
    // The bin layout depends on nothing an engine holds, so the two plots bin a
    // region identically and the second must find its answer already there.
    const TestFrame frame = texturedFrame();
    HistogramBins shared;
    Histogram banded;
    Histogram overlaid;
    banded.lendBins(&shared);
    overlaid.lendBins(&shared);
    HistogramSettings combined;
    combined.style = HistogramStyle::Combined;
    overlaid.configure(combined);

    banded.accumulate(frame.view(), IntRect{0, 0, 200, 120});
    overlaid.accumulate(frame.view(), IntRect{0, 0, 200, 120});

    CHECK(shared.scatters() == 1);
}

TEST_CASE("A shared histogram scatter draws exactly what an unshared one draws")
{
    // The condition on the whole optimization. Both images are compared byte
    // for byte against the same scope reading bins of its own, because a plot
    // that is merely close is a different measurement.
    const TestFrame frame = texturedFrame();
    const std::vector<uint8_t> bandedAlone = unsharedImage(frame, HistogramStyle::PerChannel);
    const std::vector<uint8_t> overlaidAlone = unsharedImage(frame, HistogramStyle::Combined);

    HistogramBins shared;
    Histogram banded;
    Histogram overlaid;
    banded.lendBins(&shared);
    overlaid.lendBins(&shared);
    HistogramSettings combined;
    combined.style = HistogramStyle::Combined;
    overlaid.configure(combined);
    banded.accumulate(frame.view(), IntRect{0, 0, 200, 120});
    overlaid.accumulate(frame.view(), IntRect{0, 0, 200, 120});

    CHECK(banded.image().rgba == bandedAlone);
    CHECK(overlaid.image().rgba == overlaidAlone);
}

TEST_CASE("A histogram sampling more thinly does not read the denser scatter")
{
    // The two scopes carry their own sampling strides, so one can genuinely ask
    // for fewer pixels than the other. Its answer is a different measurement
    // and must not be served the one already in the bins.
    const TestFrame frame = texturedFrame();
    const std::vector<uint8_t> thinAlone = [&] {
        HistogramSettings thin;
        thin.samplingStride = 4;
        Histogram scope;
        scope.configure(thin);
        scope.accumulate(frame.view(), IntRect{0, 0, 200, 120});

        return scope.image().rgba;
    }();

    HistogramBins shared;
    Histogram dense;
    Histogram sparse;
    dense.lendBins(&shared);
    sparse.lendBins(&shared);
    HistogramSettings thin;
    thin.samplingStride = 4;
    sparse.configure(thin);
    dense.accumulate(frame.view(), IntRect{0, 0, 200, 120});
    sparse.accumulate(frame.view(), IntRect{0, 0, 200, 120});

    CHECK(shared.scatters() == 2);
    CHECK(sparse.image().rgba == thinAlone);
}

TEST_CASE("A second frame is scattered into the histogram's bins again")
{
    // The bins answer for one frame. The next one must reach them however
    // little the pixels moved, so the key carries the frame's own identity and
    // not merely the geometry that was asked for.
    const TestFrame frame = texturedFrame();
    HistogramBins shared;
    Histogram scope;
    scope.lendBins(&shared);

    FrameView view = frame.view();
    scope.accumulate(view, IntRect{0, 0, 200, 120});
    CHECK(shared.scatters() == 1);

    view.sequence = 2;
    scope.accumulate(view, IntRect{0, 0, 200, 120});
    CHECK(shared.scatters() == 2);
}

TEST_CASE("Another buffer is scattered into the histogram's bins whatever it is numbered")
{
    // The frame's number is the producer's promise that its pixels moved on,
    // and both capture backends keep it. The bins do not rest on that promise
    // alone: pixels somewhere else are a different frame however they are
    // numbered, which is the case a test or a second producer can reach.
    const TestFrame first = texturedFrame();
    TestFrame second(200, 120, 0);
    second.fill(Color{200, 30, 90});

    HistogramBins shared;
    Histogram scope;
    scope.lendBins(&shared);
    scope.accumulate(first.view(), IntRect{0, 0, 200, 120});
    const std::vector<uint8_t> textured = scope.image().rgba;
    scope.accumulate(second.view(), IntRect{0, 0, 200, 120});

    CHECK(shared.scatters() == 2);
    CHECK(scope.image().rgba != textured);
}

TEST_CASE("A carried region is scattered into the histogram's bins again")
{
    // Scopes share only what they measure over the SAME region. The region is
    // CARRIED here rather than resized, which is what a user dragging one does
    // and the one case the sampling grid cannot stand in for: a grid is decided
    // by a region's size and says nothing about where it sits.
    const TestFrame frame = texturedFrame();
    HistogramBins shared;
    Histogram scope;
    scope.lendBins(&shared);

    scope.accumulate(frame.view(), IntRect{0, 0, 100, 60});
    const std::vector<uint8_t> atOrigin = scope.image().rgba;
    scope.accumulate(frame.view(), IntRect{40, 20, 100, 60});

    CHECK(shared.scatters() == 2);
    CHECK(scope.image().rgba != atOrigin);
}

TEST_CASE("Histogram places uniform color at its channel values")
{
    TestFrame frame(32, 32, 255);
    frame.fillColumns(0, 32, Color{10, 150, 240});

    Histogram scope;
    HistogramSettings fullHeight;
    fullHeight.style = HistogramStyle::Combined;
    scope.configure(fullHeight);
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 32});

    // Smoothing spreads one bin's population to its neighbors; the true
    // value stays the tallest.
    // Smoothing and interpolation spread one bin's population to its
    // neighborhood; the true value stays the tallest.
    CHECK(litValues(scope.image(), 0).front() >= 7);
    CHECK(litValues(scope.image(), 0).back() <= 13);
    CHECK(barHeight(scope.image(), 10, 0) >= Histogram::Height - 20);
    CHECK(barHeight(scope.image(), 9, 0) < barHeight(scope.image(), 10, 0));
    CHECK(barHeight(scope.image(), 8, 0) < barHeight(scope.image(), 9, 0));
    CHECK(barHeight(scope.image(), 150, 0) == 0);
}

TEST_CASE("Histogram bins a tall region the same when it splits across threads")
{
    // A region tall enough to split across worker threads must bin each
    // channel at its value exactly as the single-threaded path: the privatized
    // per-thread bins merge back by integer addition.
    TestFrame frame(32, 1024, 255);
    frame.fillColumns(0, 32, Color{10, 150, 240});

    Histogram scope;
    HistogramSettings combined;
    combined.style = HistogramStyle::Combined;
    scope.configure(combined);
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 1024});

    CHECK(barHeight(scope.image(), 10, 0) >= Histogram::Height - 20);
    CHECK(barHeight(scope.image(), 150, 1) >= Histogram::Height - 20);
    CHECK(barHeight(scope.image(), 240, 2) >= Histogram::Height - 20);
    CHECK(barHeight(scope.image(), 150, 0) == 0);
}

TEST_CASE("Histogram bar heights order by pixel population")
{
    // Three quarters at gray 64, one quarter at gray 200.
    TestFrame frame(64, 32, 255);
    frame.fillColumns(0, 48, Color{64, 64, 64});
    frame.fillColumns(48, 64, Color{200, 200, 200});

    Histogram scope;
    HistogramSettings fullHeight;
    fullHeight.style = HistogramStyle::Combined;
    scope.configure(fullHeight);
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 32});

    const int dominant = barHeight(scope.image(), 64, 1);
    const int minority = barHeight(scope.image(), 200, 1);
    CHECK(dominant >= Histogram::Height - 20);
    CHECK(minority > 0);
    CHECK(minority < dominant);
    // Square-root heights: a third of the pixels draws at sqrt(1/3) of
    // the height - spikes stay distinct, tails stay visible.
    CHECK(minority > Histogram::Height / 2);
    CHECK(minority < Histogram::Height * 3 / 4);
}

TEST_CASE("Histogram is invariant to sampling stride and region size")
{
    // 3:1 color mix arranged so both strides and the half-width region see
    // the same ratio; per-sample normalization must yield identical images.
    TestFrame frame(64, 64, 255);
    frame.fillColumns(0, 48, Color{64, 64, 64});
    frame.fillColumns(48, 64, Color{200, 200, 200});

    Histogram reference;
    reference.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    Histogram strided;
    HistogramSettings settings;
    settings.samplingStride = 2;
    strided.configure(settings);
    strided.accumulate(frame.view(), IntRect{0, 0, 64, 64});

    CHECK(reference.image().rgba == strided.image().rgba);
}

TEST_CASE("Histogram keeps sparse tones readable under a dominant one")
{
    // A broad tonal ramp two rows deep under a flat-sky tone sixty-four
    // times as populated per bin. Linear heights would draw the ramp as a
    // few pixels at the floor - the per-image-zoom complaint - while the
    // square root keeps it plainly visible.
    TestFrame frame(256, 10, 255);
    for (int value = 0; value < 240; ++value) {
        const auto tone = static_cast<uint8_t>(value);
        frame.fillColumns(value, value + 1, Color{tone, tone, tone});
    }
    for (int py = 2; py < 10; ++py) {
        for (int px = 0; px < 256; ++px) {
            uint8_t* p = frame.pixels.data() + (static_cast<std::size_t>(py) * 256 + px) * 4;
            p[0] = p[1] = p[2] = 200;
        }
    }

    Histogram scope;
    HistogramSettings fullHeight;
    fullHeight.style = HistogramStyle::Combined;
    scope.configure(fullHeight);
    scope.accumulate(frame.view(), IntRect{0, 0, 256, 10});

    CHECK(barHeight(scope.image(), 200, 1) >= Histogram::Height - 20);
    CHECK(barHeight(scope.image(), 120, 1) > Histogram::Height / 20);
}

TEST_CASE("Histogram per-channel style separates the channels into bands")
{
    TestFrame frame(32, 32, 255);
    frame.fillColumns(0, 32, Color{10, 150, 240});

    Histogram scope;
    HistogramSettings settings;
    settings.style = HistogramStyle::PerChannel;
    scope.configure(settings);
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 32});

    // Red in the top band only, green in the middle, blue in the bottom:
    // each channel's plot stays within its own third.
    const ScopeImage& image = scope.image();
    const int band = Histogram::Height / 3;
    const auto bandOfPeak = [&](int value, int channel) {
        const int columnsPerBin = image.width / Histogram::Bins;
        for (int row = 0; row < image.height; ++row) {
            for (int column = value * columnsPerBin; column < (value + 1) * columnsPerBin; ++column) {
                if (image.rgba[(static_cast<std::size_t>(row) * image.width + column) * 4 + channel] > 0) {
                    return row / band;
                }
            }
        }
        return -1;
    };
    CHECK(bandOfPeak(10, 0) == 0);
    CHECK(bandOfPeak(150, 1) == 1);
    CHECK(bandOfPeak(240, 2) == 2);
}

TEST_CASE("Histogram produces an empty plot for an empty region")
{
    TestFrame frame(32, 32, 255);
    frame.fillColumns(0, 32, Color{80, 80, 80});

    Histogram scope;
    scope.accumulate(frame.view(), IntRect{100, 100, 4, 4});

    for (int channel = 0; channel < 3; ++channel) {
        CHECK(litValues(scope.image(), channel).empty());
    }
}

TEST_CASE("Histogram exports its curve for display-resolution stroking")
{
    TestFrame frame(32, 32, 255);
    frame.fillColumns(0, 32, Color{10, 150, 240});

    Histogram scope;
    scope.accumulate(frame.view(), IntRect{0, 0, 32, 32});

    const std::vector<float>& outline = scope.outlineHeights();
    REQUIRE(outline.size() == static_cast<std::size_t>(3) * Histogram::Bins);
    // Each channel's curve peaks at its value, at full normalized scale
    // regardless of the style.
    const auto peakBin = [&](int channel) {
        const std::size_t plane = static_cast<std::size_t>(channel) * Histogram::Bins;
        int best = 0;
        for (int value = 0; value < Histogram::Bins; ++value) {
            if (outline[plane + value] > outline[plane + best]) {
                best = value;
            }
        }
        return best;
    };
    CHECK(std::abs(peakBin(0) - 10) <= 1);
    CHECK(std::abs(peakBin(1) - 150) <= 1);
    CHECK(std::abs(peakBin(2) - 240) <= 1);
    CHECK(outline[static_cast<std::size_t>(peakBin(0))] > 0.9f);
}

TEST_CASE("Histogram clamps its image dimensions to the supported range")
{
    TestFrame frame(32, 32, 255);
    frame.fillColumns(0, 32, Color{80, 80, 80});

    Histogram tooLarge;
    HistogramSettings large;
    large.imageWidth = 9999;
    large.imageHeight = 9999;
    tooLarge.configure(large);
    tooLarge.accumulate(frame.view(), IntRect{0, 0, 32, 32});
    CHECK(tooLarge.image().width == 4096);
    CHECK(tooLarge.image().height == 1536);

    Histogram tooSmall;
    HistogramSettings small;
    small.imageWidth = 10;
    small.imageHeight = 10;
    tooSmall.configure(small);
    tooSmall.accumulate(frame.view(), IntRect{0, 0, 32, 32});
    CHECK(tooSmall.image().width == 256);
    CHECK(tooSmall.image().height == 192);
}

TEST_CASE("Histogram leaves a gap between two populated tones dark")
{
    // Two well-separated tones with nothing between them: bins in the gap
    // stay empty rather than being bridged by the smoothing or the spline.
    TestFrame frame(64, 32, 255);
    frame.fillColumns(0, 32, Color{40, 40, 40});
    frame.fillColumns(32, 64, Color{210, 210, 210});

    Histogram scope;
    HistogramSettings combined;
    combined.style = HistogramStyle::Combined;
    scope.configure(combined);
    scope.accumulate(frame.view(), IntRect{0, 0, 64, 32});

    CHECK(barHeight(scope.image(), 40, 1) > 0);
    CHECK(barHeight(scope.image(), 210, 1) > 0);
    CHECK(barHeight(scope.image(), 125, 1) == 0);  // squarely in the gap
}

TEST_CASE("The histogram thins to its own budget, not the global ceiling")
{
    // The histogram's bins are fixed at 768 whatever the region, so its budget is
    // an order of magnitude below the global ceiling and it thins much harder
    // than any other scope. Nothing observable changes if that wiring is reverted
    // - only the cost - so this is the only thing standing between the saving and
    // a silent regression.
    constexpr int Width = 3000;
    constexpr int Height = 1500;
    const IntRect region{0, 0, Width, Height};
    const long long own = budgetForBins(256LL * 3, HistogramMinSamplesPerBin);
    const SampleGrid mine = sampleGridFor(1, region, own);
    const SampleGrid global = sampleGridFor(1, region, SampleBudget);
    // The two must disagree, or this frame cannot tell them apart.
    REQUIRE(mine.rowStride > global.rowStride);

    // Black on exactly the rows the histogram's own budget visits, white on the
    // rest. Sampling to its own budget sees one code; sampling to the global one
    // sees both, and lights both ends of the code axis.
    TestFrame frame(Width, Height, 0);
    frame.fill(Color{255, 255, 255});
    for (int index = 0; index < mine.rows; ++index) {
        const int row = sampleRowOf(mine, region, index);
        frame.fillRows(row, row + 1, Color{0, 0, 0});
    }

    Histogram histogram;
    histogram.configure(HistogramSettings{});
    histogram.accumulate(frame.view(), region);

    const ScopeImage& image = histogram.image();
    const int edge = std::max(1, image.width / 50);
    const auto litWithin = [&image](int from, int to) {
        for (int px = from; px < to; ++px) {
            for (int py = 0; py < image.height; ++py) {
                if (pixelLit(image, px, py)) {
                    return true;
                }
            }
        }

        return false;
    };

    // Whichever end code zero sits at, exactly one end carries mass - never both.
    CHECK(litWithin(0, edge) != litWithin(image.width - edge, image.width));
}

}  // namespace sidescopes
