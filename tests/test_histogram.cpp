#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <vector>

#include "core/scopes/histogram.h"

namespace sidescopes {
namespace {

struct TestFrame
{
    explicit TestFrame(int width, int height)
        : width(width),
          height(height)
    {
        pixels.resize(static_cast<std::size_t>(width) * height * 4, 255);
    }

    void fillColumns(int x0, int x1, Color color)
    {
        for (int py = 0; py < height; ++py) {
            for (int px = x0; px < x1; ++px) {
                uint8_t* p = pixels.data() + (static_cast<std::size_t>(py) * width + px) * 4;
                p[0] = color.b;
                p[1] = color.g;
                p[2] = color.r;
            }
        }
    }

    [[nodiscard]] FrameView view() const
    {
        return FrameView{pixels.data(), width * 4, width, height, ColorSpaceHint::Srgb, 1};
    }

    std::vector<uint8_t> pixels;
    int width;
    int height;
};

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

}  // namespace

TEST_CASE("Histogram places uniform color at its channel values")
{
    TestFrame frame(32, 32);
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

TEST_CASE("Histogram bar heights order by pixel population")
{
    // Three quarters at gray 64, one quarter at gray 200.
    TestFrame frame(64, 32);
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
    TestFrame frame(64, 64);
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
    TestFrame frame(256, 10);
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
    TestFrame frame(32, 32);
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
    TestFrame frame(32, 32);
    frame.fillColumns(0, 32, Color{80, 80, 80});

    Histogram scope;
    scope.accumulate(frame.view(), IntRect{100, 100, 4, 4});

    for (int channel = 0; channel < 3; ++channel) {
        CHECK(litValues(scope.image(), channel).empty());
    }
}

TEST_CASE("Histogram exports its curve for display-resolution stroking")
{
    TestFrame frame(32, 32);
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

}  // namespace sidescopes
