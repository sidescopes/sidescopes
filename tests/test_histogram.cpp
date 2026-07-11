#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/scopes/histogram.h"

namespace sidescopes {
namespace {

struct TestFrame {
    explicit TestFrame(int width, int height) : width(width), height(height) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4, 255);
    }

    void FillColumns(int x0, int x1, Color color) {
        for (int py = 0; py < height; ++py) {
            for (int px = x0; px < x1; ++px) {
                uint8_t* p = pixels.data() + (static_cast<std::size_t>(py) * width + px) * 4;
                p[0] = color.b;
                p[1] = color.g;
                p[2] = color.r;
            }
        }
    }

    [[nodiscard]] FrameView View() const {
        return FrameView{pixels.data(), width * 4, width, height, ColorSpaceHint::Srgb, 1};
    }

    std::vector<uint8_t> pixels;
    int width;
    int height;
};

// Plot height for a bin in one channel (0=r, 1=g, 2=b): the tallest of
// the image columns that render that bin.
int BarHeight(const ScopeImage& image, int value, int channel) {
    const int columns_per_bin = image.width / Histogram::kBins;
    int height = 0;
    for (int column = value * columns_per_bin; column < (value + 1) * columns_per_bin; ++column) {
        int column_height = 0;
        for (int row = 0; row < image.height; ++row) {
            if (image.rgba[(static_cast<std::size_t>(row) * image.width + column) * 4 + channel] >
                0)
                ++column_height;
        }
        height = std::max(height, column_height);
    }
    return height;
}

// Bins with a nonzero plot in one channel.
std::vector<int> LitValues(const ScopeImage& image, int channel) {
    std::vector<int> values;
    for (int value = 0; value < Histogram::kBins; ++value) {
        if (BarHeight(image, value, channel) > 0) values.push_back(value);
    }
    return values;
}

}  // namespace

TEST_CASE("Histogram places uniform color at its channel values") {
    TestFrame frame(32, 32);
    frame.FillColumns(0, 32, Color{10, 150, 240});

    Histogram scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 32});

    // Smoothing spreads one bin's population to its neighbors; the true
    // value stays the tallest.
    // Smoothing and interpolation spread one bin's population to its
    // neighborhood; the true value stays the tallest.
    CHECK(LitValues(scope.Image(), 0).front() >= 7);
    CHECK(LitValues(scope.Image(), 0).back() <= 13);
    CHECK(BarHeight(scope.Image(), 10, 0) >= Histogram::kHeight - 20);
    CHECK(BarHeight(scope.Image(), 9, 0) < BarHeight(scope.Image(), 10, 0));
    CHECK(BarHeight(scope.Image(), 8, 0) < BarHeight(scope.Image(), 9, 0));
    CHECK(BarHeight(scope.Image(), 150, 0) == 0);
}

TEST_CASE("Histogram bar heights order by pixel population") {
    // Three quarters at gray 64, one quarter at gray 200.
    TestFrame frame(64, 32);
    frame.FillColumns(0, 48, Color{64, 64, 64});
    frame.FillColumns(48, 64, Color{200, 200, 200});

    Histogram scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 64, 32});

    const int dominant = BarHeight(scope.Image(), 64, 1);
    const int minority = BarHeight(scope.Image(), 200, 1);
    CHECK(dominant >= Histogram::kHeight - 20);
    CHECK(minority > 0);
    CHECK(minority < dominant);
    // Square-root heights: a third of the pixels draws at sqrt(1/3) of
    // the height - spikes stay distinct, tails stay visible.
    CHECK(minority > Histogram::kHeight / 2);
    CHECK(minority < Histogram::kHeight * 3 / 4);
}

TEST_CASE("Histogram is invariant to sampling stride and region size") {
    // 3:1 color mix arranged so both strides and the half-width region see
    // the same ratio; per-sample normalization must yield identical images.
    TestFrame frame(64, 64);
    frame.FillColumns(0, 48, Color{64, 64, 64});
    frame.FillColumns(48, 64, Color{200, 200, 200});

    Histogram reference;
    reference.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    Histogram strided;
    HistogramSettings settings;
    settings.sampling_stride = 2;
    strided.Configure(settings);
    strided.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    CHECK(reference.Image().rgba == strided.Image().rgba);
}

TEST_CASE("Histogram keeps sparse tones readable under a dominant one") {
    // A broad tonal ramp two rows deep under a flat-sky tone sixty-four
    // times as populated per bin. Linear heights would draw the ramp as a
    // few pixels at the floor - the per-image-zoom complaint - while the
    // square root keeps it plainly visible.
    TestFrame frame(256, 10);
    for (int value = 0; value < 240; ++value) {
        const auto tone = static_cast<uint8_t>(value);
        frame.FillColumns(value, value + 1, Color{tone, tone, tone});
    }
    for (int py = 2; py < 10; ++py)
        for (int px = 0; px < 256; ++px) {
            uint8_t* p = frame.pixels.data() + (static_cast<std::size_t>(py) * 256 + px) * 4;
            p[0] = p[1] = p[2] = 200;
        }

    Histogram scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 256, 10});

    CHECK(BarHeight(scope.Image(), 200, 1) >= Histogram::kHeight - 20);
    CHECK(BarHeight(scope.Image(), 120, 1) > Histogram::kHeight / 20);
}

TEST_CASE("Histogram per-channel style separates the channels into bands") {
    TestFrame frame(32, 32);
    frame.FillColumns(0, 32, Color{10, 150, 240});

    Histogram scope;
    HistogramSettings settings;
    settings.style = HistogramStyle::PerChannel;
    scope.Configure(settings);
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 32});

    // Red in the top band only, green in the middle, blue in the bottom:
    // each channel's plot stays within its own third.
    const ScopeImage& image = scope.Image();
    const int band = Histogram::kHeight / 3;
    const auto band_of_peak = [&](int value, int channel) {
        const int columns_per_bin = image.width / Histogram::kBins;
        for (int row = 0; row < image.height; ++row)
            for (int column = value * columns_per_bin; column < (value + 1) * columns_per_bin;
                 ++column)
                if (image.rgba[(static_cast<std::size_t>(row) * image.width + column) * 4 +
                               channel] > 0)
                    return row / band;
        return -1;
    };
    CHECK(band_of_peak(10, 0) == 0);
    CHECK(band_of_peak(150, 1) == 1);
    CHECK(band_of_peak(240, 2) == 2);
}

TEST_CASE("Histogram produces an empty plot for an empty region") {
    TestFrame frame(32, 32);
    frame.FillColumns(0, 32, Color{80, 80, 80});

    Histogram scope;
    scope.Accumulate(frame.View(), IntRect{100, 100, 4, 4});

    for (int channel = 0; channel < 3; ++channel) CHECK(LitValues(scope.Image(), channel).empty());
}

}  // namespace sidescopes
