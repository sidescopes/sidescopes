#include <array>
#include <catch2/catch_test_macros.hpp>

#include "core/frame.h"

namespace sidescopes {

TEST_CASE("IntRect reports emptiness") {
    CHECK(IntRect{}.Empty());
    CHECK(IntRect{0, 0, 10, 0}.Empty());
    CHECK(IntRect{0, 0, 0, 10}.Empty());
    CHECK(IntRect{5, 5, -3, 4}.Empty());
    CHECK_FALSE(IntRect{0, 0, 1, 1}.Empty());
}

TEST_CASE("IntRect clamps to frame bounds") {
    SECTION("a rect inside the frame is unchanged") {
        const IntRect clamped = IntRect{10, 20, 30, 40}.ClampedTo(100, 100);
        CHECK(clamped.x == 10);
        CHECK(clamped.y == 20);
        CHECK(clamped.width == 30);
        CHECK(clamped.height == 40);
    }

    SECTION("negative origin is cut, not shifted") {
        const IntRect clamped = IntRect{-10, -5, 30, 30}.ClampedTo(100, 100);
        CHECK(clamped.x == 0);
        CHECK(clamped.y == 0);
        CHECK(clamped.width == 20);
        CHECK(clamped.height == 25);
    }

    SECTION("overhang past the frame edge is trimmed") {
        const IntRect clamped = IntRect{90, 95, 30, 30}.ClampedTo(100, 100);
        CHECK(clamped.width == 10);
        CHECK(clamped.height == 5);
    }

    SECTION("a rect entirely outside the frame becomes empty") {
        CHECK(IntRect{200, 200, 10, 10}.ClampedTo(100, 100).Empty());
        CHECK(IntRect{-50, 0, 20, 20}.ClampedTo(100, 100).Empty());
    }
}

TEST_CASE("FrameView reads BGRA pixels as RGB colors") {
    // A 2x2 frame with one pixel per corner color, plus per-row padding to
    // exercise the stride path.
    constexpr int kStride = 2 * 4 + 8;
    std::array<uint8_t, static_cast<std::size_t>(2) * kStride> pixels{};
    const auto write_bgra = [&](int px, int py, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = pixels.data() + static_cast<std::ptrdiff_t>(py) * kStride +
                     static_cast<std::ptrdiff_t>(px) * 4;
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = 255;
    };
    write_bgra(0, 0, 255, 0, 0);
    write_bgra(1, 0, 0, 255, 0);
    write_bgra(0, 1, 0, 0, 255);
    write_bgra(1, 1, 17, 34, 51);

    const FrameView view{pixels.data(), kStride, 2, 2, ColorSpaceHint::Srgb, 1};

    CHECK(view.ColorAt(0, 0).r == 255);
    CHECK(view.ColorAt(1, 0).g == 255);
    CHECK(view.ColorAt(0, 1).b == 255);
    const Color mixed = view.ColorAt(1, 1);
    CHECK(mixed.r == 17);
    CHECK(mixed.g == 34);
    CHECK(mixed.b == 51);
}

}  // namespace sidescopes
