#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>

#include "app/capture_crop.h"

namespace sidescopes {
namespace {

// A settled canvas-sized region on a 4K display, with nothing else reading a
// frame: the case narrowing exists for.
CropInputs settledCanvas()
{
    CropInputs inputs;
    inputs.region = IntRect{200, 150, 1500, 1000};
    inputs.displayWidth = 3840;
    inputs.displayHeight = 2160;
    inputs.regionChangedAt = 0.0;
    inputs.now = CropSettleSeconds * 2.0;

    return inputs;
}

}  // namespace

TEST_CASE("A settled region narrows the capture to itself")
{
    const std::optional<IntRect> crop = cropFor(settledCanvas());
    REQUIRE(crop.has_value());
    CHECK(crop->x == 200);
    CHECK(crop->y == 150);
    CHECK(crop->width == 1500);
    CHECK(crop->height == 1000);
}

TEST_CASE("Anything that reads outside the region keeps the whole display")
{
    // The picker scans displays for windows and faces, and averages a dragged
    // pin's area, all of which can land anywhere; a face lock's probe reads the
    // active window's rectangle, which is not the analysis region.
    CropInputs picker = settledCanvas();
    picker.pickerActive = true;
    CHECK_FALSE(cropFor(picker).has_value());

    CropInputs faces = settledCanvas();
    faces.faceLockActive = true;
    CHECK_FALSE(cropFor(faces).has_value());
}

TEST_CASE("A region still moving is not narrowed to")
{
    // Narrowing reconfigures a running stream, so a region under the cursor - or
    // an attached window mid-drag - would pay for one reconfiguration a frame.
    CropInputs moving = settledCanvas();
    moving.now = moving.regionChangedAt + CropSettleSeconds * 0.5;
    CHECK_FALSE(cropFor(moving).has_value());

    // And is narrowed to once it holds still.
    moving.now = moving.regionChangedAt + CropSettleSeconds + 0.01;
    CHECK(cropFor(moving).has_value());
}

TEST_CASE("A region covering nearly the whole display is left alone")
{
    // The saving would be a few percent and the reconfiguration is not free, so
    // there is a share above which narrowing is not worth asking for.
    CropInputs wide = settledCanvas();
    wide.region = IntRect{0, 0, 3840, 2000};
    CHECK_FALSE(cropFor(wide).has_value());

    // Just under the threshold still narrows: the rule is a share, not a guess.
    wide.region = IntRect{0, 0, 3840, static_cast<int>(2160 * 0.7)};
    CHECK(cropFor(wide).has_value());
}

TEST_CASE("An unusable region or display asks for no narrowing")
{
    CropInputs empty = settledCanvas();
    empty.region = IntRect{};
    CHECK_FALSE(cropFor(empty).has_value());

    CropInputs offDisplay = settledCanvas();
    offDisplay.region = IntRect{5000, 5000, 100, 100};
    CHECK_FALSE(cropFor(offDisplay).has_value());

    CropInputs noDisplay = settledCanvas();
    noDisplay.displayWidth = 0;
    CHECK_FALSE(cropFor(noDisplay).has_value());
}

TEST_CASE("A region hanging off the display is narrowed to the part that exists")
{
    // The compositor would reject a rectangle reaching past the display, and a
    // region can outlive the window it followed off the edge.
    CropInputs overhang = settledCanvas();
    overhang.region = IntRect{3800, 2100, 400, 400};
    const std::optional<IntRect> crop = cropFor(overhang);
    REQUIRE(crop.has_value());
    CHECK(crop->x == 3800);
    CHECK(crop->y == 2100);
    CHECK(crop->width == 40);
    CHECK(crop->height == 60);
}

TEST_CASE("A reader of the whole display can tell a narrowed frame apart")
{
    std::array<uint8_t, std::size_t{4} * 4 * 4> pixels{};
    const FrameView whole{pixels.data(), 4 * 4, 4, 4, ColorSpaceHint::Srgb, 1};
    CHECK(coversWholeDisplay(whole));

    const FrameView narrowed{pixels.data(), 4 * 4, 4, 4, ColorSpaceHint::Srgb, 1, 10, 10, 100, 100};
    CHECK_FALSE(coversWholeDisplay(narrowed));
}

}  // namespace sidescopes
