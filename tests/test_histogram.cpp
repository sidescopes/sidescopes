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

// Height of the bar for `value` in one channel (0=r, 1=g, 2=b).
int BarHeight(const ScopeImage& image, int value, int channel) {
    int height = 0;
    for (int row = 0; row < image.height; ++row) {
        if (image.rgba[(static_cast<std::size_t>(row) * image.width + value) * 4 + channel] > 0)
            ++height;
    }
    return height;
}

// Values with a nonzero bar in one channel.
std::vector<int> LitValues(const ScopeImage& image, int channel) {
    std::vector<int> values;
    for (int value = 0; value < image.width; ++value) {
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

    CHECK(LitValues(scope.Image(), 0) == std::vector<int>{10});
    CHECK(LitValues(scope.Image(), 1) == std::vector<int>{150});
    CHECK(LitValues(scope.Image(), 2) == std::vector<int>{240});
    // The only bar is the densest bar: full height.
    CHECK(BarHeight(scope.Image(), 10, 0) == Histogram::kHeight);
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
    CHECK(dominant == Histogram::kHeight);
    CHECK(minority > 0);
    CHECK(minority < dominant);
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

TEST_CASE("Histogram produces an empty plot for an empty region") {
    TestFrame frame(32, 32);
    frame.FillColumns(0, 32, Color{80, 80, 80});

    Histogram scope;
    scope.Accumulate(frame.View(), IntRect{100, 100, 4, 4});

    for (int channel = 0; channel < 3; ++channel) CHECK(LitValues(scope.Image(), channel).empty());
}

}  // namespace sidescopes
