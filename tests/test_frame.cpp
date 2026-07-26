#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>

#include "core/analysis_worker.h"
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

TEST_CASE("A frame that covers its whole display reports itself as the display")
{
    std::array<uint8_t, std::size_t{4} * 4 * 4> pixels{};
    const FrameView whole{pixels.data(), 4 * 4, 4, 4, ColorSpaceHint::Srgb, 1};

    CHECK(whole.displayWidth() == 4);
    CHECK(whole.displayHeight() == 4);
    CHECK_FALSE(whole.cropped());
    // The identity, which is what keeps an uncropped capture behaving exactly
    // as it did before frames could describe a crop.
    const IntRect rect{1, 2, 2, 1};
    const IntRect mapped = whole.fromDisplay(rect);
    CHECK(mapped.x == rect.x);
    CHECK(mapped.y == rect.y);
    CHECK(mapped.width == rect.width);
    CHECK(mapped.height == rect.height);
}

TEST_CASE("A narrowed frame maps display coordinates onto its own pixels")
{
    // Forty by thirty pixels taken from (100, 60) of a 1000x800 display.
    std::array<uint8_t, std::size_t{40} * 30 * 4> pixels{};
    const FrameView crop{pixels.data(), 40 * 4, 40, 30, ColorSpaceHint::Srgb, 7, 100, 60, 1000, 800};

    CHECK(crop.displayWidth() == 1000);
    CHECK(crop.displayHeight() == 800);
    CHECK(crop.cropped());

    // A rectangle stated in display pixels lands where those pixels actually
    // are inside this frame.
    const IntRect mapped = crop.fromDisplay(IntRect{110, 70, 20, 10});
    CHECK(mapped.x == 10);
    CHECK(mapped.y == 10);
    CHECK(mapped.width == 20);
    CHECK(mapped.height == 10);

    // The crop's own origin maps to its top-left corner.
    CHECK(crop.fromDisplay(IntRect{100, 60, 1, 1}).x == 0);
    CHECK(crop.fromDisplay(IntRect{100, 60, 1, 1}).y == 0);
}

TEST_CASE("A narrowed frame resolves a region to the same display pixels")
{
    // The whole point of the mapping: a region stated as a share of the display
    // must select the same content whether or not the capture was narrowed. Here
    // the region is the middle fifth of a 1000x800 display, and the capture has
    // been narrowed to exactly that.
    RegionOfInterest middle;
    middle.leftPercent = 40.0;
    middle.topPercent = 40.0;
    middle.rightPercent = 60.0;
    middle.bottomPercent = 60.0;

    const IntRect onDisplay = middle.toPixels(1000, 800);
    CHECK(onDisplay.x == 400);
    CHECK(onDisplay.y == 320);
    CHECK(onDisplay.width == 200);
    CHECK(onDisplay.height == 160);

    std::array<uint8_t, 8> tiny{};
    const FrameView narrowed{tiny.data(), 200 * 4, 200, 160, ColorSpaceHint::Srgb, 3, 400, 320, 1000, 800};
    // Resolved against the display, then moved into the frame: the whole frame.
    const IntRect inFrame = narrowed.fromDisplay(middle.toPixels(narrowed.displayWidth(), narrowed.displayHeight()));
    CHECK(inFrame.x == 0);
    CHECK(inFrame.y == 0);
    CHECK(inFrame.width == 200);
    CHECK(inFrame.height == 160);

    // Read against the frame's own extents instead - the bug this guards - and
    // the region would collapse to a fifth of the crop, well inside it.
    const IntRect wrong = middle.toPixels(narrowed.width, narrowed.height);
    CHECK(wrong.width == 40);
    CHECK(wrong.x == 80);
}

TEST_CASE("A frame says whether it carries a rectangle of its display")
{
    std::array<uint8_t, 8> tiny{};
    // A 200x160 crop at 400,320 of a 1000x800 display.
    const FrameView narrowed{tiny.data(), 200 * 4, 200, 160, ColorSpaceHint::Srgb, 1, 400, 320, 1000, 800};

    CHECK(narrowed.carries(IntRect{400, 320, 200, 160}));
    CHECK(narrowed.carries(IntRect{450, 350, 50, 50}));
    // Straddling an edge is not carrying: the part outside is simply absent,
    // and clipping to the overlap is what publishes a fraction of a region as
    // if it were all of it.
    CHECK_FALSE(narrowed.carries(IntRect{350, 320, 200, 160}));
    CHECK_FALSE(narrowed.carries(IntRect{400, 320, 260, 160}));
    CHECK_FALSE(narrowed.carries(IntRect{0, 0, 100, 100}));

    // An uncropped frame carries anything inside its display, which is itself.
    const FrameView whole{tiny.data(), 200 * 4, 200, 160, ColorSpaceHint::Srgb, 1};
    CHECK(whole.carries(IntRect{0, 0, 200, 160}));
    CHECK(whole.carries(IntRect{10, 10, 20, 20}));
}

}  // namespace sidescopes
