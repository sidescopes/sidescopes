#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/scopes/vectorscope.h"

namespace sidescopes {
namespace {

// Builds a tightly-packed BGRA frame filled by a per-pixel color function.
struct TestFrame {
    explicit TestFrame(int width, int height) : width(width), height(height) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4, 255);
    }

    void Fill(int x0, int x1, Color color) {
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

// The brightest pixel of an accumulation: smoothing spreads a
// single-color trace over a small neighborhood, and the peak must stay
// on the exact chroma coordinate.
std::pair<int, int> BrightestPixel(const ScopeImage& image) {
    std::pair<int, int> brightest{-1, -1};
    int best = 0;
    for (int py = 0; py < image.height; ++py) {
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* rgba =
                image.rgba.data() + (static_cast<std::size_t>(py) * image.width + px) * 4;
            const int sum = rgba[0] + rgba[1] + rgba[2];
            if (sum > best) {
                best = sum;
                brightest = {px, py};
            }
        }
    }
    return brightest;
}

}  // namespace

TEST_CASE("Vectorscope places 75% red on the classic BT.601 target") {
    // 75% red (191, 0, 0) sits at Cb = 99.65, Cr = 211.56. The bilinear
    // splat peaks on the ROUNDED position - bin (100, 255 - 212 = 43) -
    // where integer truncation used to floor it to (99, 44), half a bin
    // away from where the projection puts the markers.
    TestFrame frame(8, 8);
    frame.Fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 8, 8});

    CHECK(BrightestPixel(scope.Image()) == std::pair<int, int>{100, 43});
}

TEST_CASE("Vectorscope maps neutral gray to the center") {
    TestFrame frame(8, 8);
    frame.Fill(0, 8, Color{128, 128, 128});

    Vectorscope scope;
    scope.Accumulate(frame.View(), IntRect{0, 0, 8, 8});

    CHECK(BrightestPixel(scope.Image()) == std::pair<int, int>{128, 127});
}

TEST_CASE("Vectorscope projection agrees with accumulation") {
    Vectorscope scope;
    const auto point = scope.Project(FloatColor{191.0f, 0.0f, 0.0f});
    REQUIRE(point.has_value());
    // Floating-point chroma for 75% red: Cb = 99.65, Cr = 211.56.
    CHECK(point->x == Catch::Approx(99.65 / 255.0).margin(0.005));
    CHECK(point->y == Catch::Approx((255.0 - 211.56) / 255.0).margin(0.005));
}

TEST_CASE("Vectorscope matrix selection moves chroma targets") {
    TestFrame frame(8, 8);
    frame.Fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    VectorscopeSettings settings;
    settings.matrix = ChromaMatrix::Bt709;
    scope.Configure(settings);
    scope.Accumulate(frame.View(), IntRect{0, 0, 8, 8});

    // BT.709: Cb = -26 * 191 / 256 + 128 = 108.6, rounding to bin 109;
    // Cr unchanged from BT.601 (both use 112), peaking at row 43.
    CHECK(BrightestPixel(scope.Image()) == std::pair<int, int>{109, 43});
}

TEST_CASE("Vectorscope carries real detail on a finer grid") {
    // The fixed-point chroma transform holds more precision than the
    // classic 256 grid uses; the finer grid must place the same color on
    // the scaled coordinate rather than upscale the coarse one.
    TestFrame frame(32, 32);
    frame.Fill(0, 32, Color{191, 0, 0});

    Vectorscope scope;
    VectorscopeSettings settings;
    settings.size = 512;
    scope.Configure(settings);
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 32});

    CHECK(scope.Image().width == 512);
    const auto [px, py] = BrightestPixel(scope.Image());
    CHECK(px >= 197);
    CHECK(px <= 201);
    CHECK(py >= 86);
    CHECK(py <= 90);
}

TEST_CASE("Vectorscope leaves no gap between adjacent chroma codes on the fine grid") {
    // 8-bit content quantizes chroma to whole codes, so neighboring
    // colors in a photograph sit one code apart - two pixels on the 512
    // image. Accumulating on a grid that fine renders the quantization
    // as gridded texture; the fine image must instead interpolate the
    // code grid, keeping the space between two equally-strong adjacent
    // codes as bright as the codes themselves.
    TestFrame frame(32, 32);
    frame.Fill(0, 16, Color{191, 0, 0});   // Cb 99.65 -> code 100
    frame.Fill(16, 32, Color{191, 0, 2});  // Cb 100.52 -> code 101

    Vectorscope scope;
    VectorscopeSettings settings;
    settings.size = 512;
    scope.Configure(settings);
    scope.Accumulate(frame.View(), IntRect{0, 0, 32, 32});

    const auto [px, py] = BrightestPixel(scope.Image());
    const auto brightness = [&](int x, int y) {
        const uint8_t* pixel =
            scope.Image().rgba.data() + (static_cast<std::size_t>(y) * 512 + x) * 4;
        return static_cast<int>(pixel[0]) + pixel[1] + pixel[2];
    };
    const int peak = brightness(px, py);
    int valley = peak;
    for (int x = px - 4; x <= px + 4; ++x) valley = std::min(valley, brightness(x, py));
    CHECK(valley * 4 >= peak * 3);
}

TEST_CASE("Vectorscope trace is invariant to the sampling stride") {
    // Two colors in a 3:1 area ratio, chosen so the ratio is preserved under
    // stride-2 sampling. Per-sample density normalization must then produce
    // identical images at both strides.
    TestFrame frame(64, 64);
    frame.Fill(0, 48, Color{191, 0, 0});
    frame.Fill(48, 64, Color{0, 0, 191});

    Vectorscope full;
    Vectorscope strided;
    VectorscopeSettings settings;
    settings.sampling_stride = 2;
    strided.Configure(settings);

    full.Accumulate(frame.View(), IntRect{0, 0, 64, 64});
    strided.Accumulate(frame.View(), IntRect{0, 0, 64, 64});

    CHECK(full.Image().rgba == strided.Image().rgba);
}

TEST_CASE("Vectorscope produces a black image for an empty region") {
    TestFrame frame(8, 8);
    frame.Fill(0, 8, Color{191, 0, 0});

    Vectorscope scope;
    scope.Accumulate(frame.View(), IntRect{20, 20, 4, 4});  // outside the frame

    for (std::size_t i = 0; i < scope.Image().rgba.size(); i += 4) {
        REQUIRE(scope.Image().rgba[i] == 0);
    }
}

}  // namespace sidescopes
