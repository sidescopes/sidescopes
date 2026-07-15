#include <array>
#include <catch2/catch_test_macros.hpp>

#include "core/frame.h"

namespace sidescopes {

TEST_CASE("IntRect reports emptiness")
{
    CHECK(IntRect{}.empty());
    CHECK(IntRect{0, 0, 10, 0}.empty());
    CHECK(IntRect{0, 0, 0, 10}.empty());
    CHECK(IntRect{5, 5, -3, 4}.empty());
    CHECK_FALSE(IntRect{0, 0, 1, 1}.empty());
}

TEST_CASE("IntRect clamps to frame bounds")
{
    SECTION("a rect inside the frame is unchanged")
    {
        const IntRect clamped = IntRect{10, 20, 30, 40}.clampedTo(100, 100);
        CHECK(clamped.x == 10);
        CHECK(clamped.y == 20);
        CHECK(clamped.width == 30);
        CHECK(clamped.height == 40);
    }

    SECTION("negative origin is cut, not shifted")
    {
        const IntRect clamped = IntRect{-10, -5, 30, 30}.clampedTo(100, 100);
        CHECK(clamped.x == 0);
        CHECK(clamped.y == 0);
        CHECK(clamped.width == 20);
        CHECK(clamped.height == 25);
    }

    SECTION("overhang past the frame edge is trimmed")
    {
        const IntRect clamped = IntRect{90, 95, 30, 30}.clampedTo(100, 100);
        CHECK(clamped.width == 10);
        CHECK(clamped.height == 5);
    }

    SECTION("a rect entirely outside the frame becomes empty")
    {
        CHECK(IntRect{200, 200, 10, 10}.clampedTo(100, 100).empty());
        CHECK(IntRect{-50, 0, 20, 20}.clampedTo(100, 100).empty());
    }
}

TEST_CASE("FrameView reads BGRA pixels as RGB colors")
{
    // A 2x2 frame with one pixel per corner color, plus per-row padding to
    // exercise the stride path.
    constexpr int Stride = 2 * 4 + 8;
    std::array<uint8_t, static_cast<std::size_t>(2) * Stride> pixels{};
    const auto writeBgra = [&](int px, int py, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = pixels.data() + static_cast<std::ptrdiff_t>(py) * Stride + static_cast<std::ptrdiff_t>(px) * 4;
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = 255;
    };
    writeBgra(0, 0, 255, 0, 0);
    writeBgra(1, 0, 0, 255, 0);
    writeBgra(0, 1, 0, 0, 255);
    writeBgra(1, 1, 17, 34, 51);

    const FrameView view{pixels.data(), Stride, 2, 2, ColorSpaceHint::Srgb, 1};

    CHECK(view.colorAt(0, 0).r == 255);
    CHECK(view.colorAt(1, 0).g == 255);
    CHECK(view.colorAt(0, 1).b == 255);
    const Color mixed = view.colorAt(1, 1);
    CHECK(mixed.r == 17);
    CHECK(mixed.g == 34);
    CHECK(mixed.b == 51);
}

}  // namespace sidescopes
