#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/region_hash.h"

namespace sidescopes {
namespace {

struct TestFrame {
    explicit TestFrame(int width, int height) : width(width), height(height) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4, 0);
    }

    void SetPixel(int px, int py, uint8_t value) {
        pixels[(static_cast<std::size_t>(py) * width + px) * 4] = value;
    }

    [[nodiscard]] FrameView View() const {
        return FrameView{pixels.data(), width * 4, width, height, ColorSpaceHint::Srgb, 1};
    }

    std::vector<uint8_t> pixels;
    int width;
    int height;
};

}  // namespace

TEST_CASE("HashRegion is stable for identical content") {
    TestFrame frame(64, 64);
    frame.SetPixel(10, 8, 200);
    const IntRect region{0, 0, 64, 64};

    CHECK(HashRegion(frame.View(), region) == HashRegion(frame.View(), region));
}

TEST_CASE("HashRegion changes when content inside the region changes") {
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 64, 64};
    const uint64_t before = HashRegion(frame.View(), region);

    frame.SetPixel(10, 8, 200);  // row 8 is sampled (multiple of 4)
    CHECK(HashRegion(frame.View(), region) != before);
}

TEST_CASE("HashRegion ignores changes outside the region") {
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 32, 32};
    const uint64_t before = HashRegion(frame.View(), region);

    frame.SetPixel(50, 48, 200);
    CHECK(HashRegion(frame.View(), region) == before);
}

TEST_CASE("HashRegion ignores changes inside the masked area") {
    TestFrame frame(64, 64);
    const IntRect region{0, 0, 64, 64};
    const IntRect masked{16, 16, 32, 32};
    const uint64_t before = HashRegion(frame.View(), region, masked);

    frame.SetPixel(31, 24, 200);  // inside the mask
    CHECK(HashRegion(frame.View(), region, masked) == before);

    frame.SetPixel(2, 24, 77);  // same row, left of the mask
    CHECK(HashRegion(frame.View(), region, masked) != before);
}

TEST_CASE("HashRegion of an empty region is stable") {
    TestFrame frame(64, 64);
    CHECK(HashRegion(frame.View(), IntRect{200, 200, 10, 10}) ==
          HashRegion(frame.View(), IntRect{300, 300, 10, 10}));
}

}  // namespace sidescopes
