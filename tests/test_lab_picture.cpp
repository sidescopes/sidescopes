#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "web/lab_picture.h"

namespace sidescopes {
namespace {

/// Writes a recognisable RGBA picture into the decode buffer, as the page
/// would: every pixel a different colour, so a byte order that got exchanged
/// twice cannot pass for one that was never exchanged at all.
void decodeInto(LabPicture& picture, int width, int height)
{
    uint8_t* into = picture.decodeInto(width, height);
    REQUIRE(into != nullptr);
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t at = 0; at < pixels; ++at) {
        into[at * 4] = static_cast<uint8_t>(10 + at);       // red
        into[at * 4 + 1] = static_cast<uint8_t>(90 + at);   // green
        into[at * 4 + 2] = static_cast<uint8_t>(170 + at);  // blue
        into[at * 4 + 3] = 255;
    }
    picture.adoptDecoded();
}

}  // namespace

TEST_CASE("The canvas and the engines are given the same pixels")
{
    // THE rule this type exists for. If the picture on screen and the pixels
    // reaching the engines could differ, the lab would teach something false
    // about what a scope measures - and would look right while doing it.
    LabPicture picture;
    decodeInto(picture, 4, 3);

    ImageAdjustments warm;
    warm.temperature = 0.6f;
    warm.exposure = 0.4f;
    CHECK(picture.setAdjustments(warm));
    CHECK(picture.refresh());

    const std::vector<uint8_t>& analysed = picture.analysed();
    const std::vector<uint8_t>& shown = picture.display().rgba;
    REQUIRE(analysed.size() == shown.size());
    for (std::size_t at = 0; at + 3 < analysed.size(); at += 4) {
        // The same pass, in the two byte orders: red and blue exchanged,
        // green and alpha where they were.
        CHECK(analysed[at] == shown[at + 2]);
        CHECK(analysed[at + 1] == shown[at + 1]);
        CHECK(analysed[at + 2] == shown[at]);
        CHECK(analysed[at + 3] == shown[at + 3]);
    }
}

TEST_CASE("Adjustments are computed from the decode, never from the last result")
{
    // Applying over the previous output would degrade the photograph as a
    // control is dragged back and forth, and the scopes would report that
    // damage as belonging to the picture.
    LabPicture picture;
    decodeInto(picture, 8, 8);
    const std::vector<uint8_t> pristine = picture.analysed();

    ImageAdjustments strong;
    strong.contrast = 0.8f;
    strong.saturation = 0.7f;
    ImageAdjustments other = strong;
    other.contrast = -0.5f;
    for (int pass = 0; pass < 20; ++pass) {
        // Back and forth, as a hand on a slider does. Each move is a real
        // change, so each one is a fresh pass over the picture.
        REQUIRE(picture.setAdjustments(other));
        REQUIRE(picture.refresh());
        REQUIRE(picture.setAdjustments(strong));
        REQUIRE(picture.refresh());
    }
    const std::vector<uint8_t> afterMuchDragging = picture.analysed();

    CHECK(picture.setAdjustments(ImageAdjustments{}));
    CHECK(picture.refresh());
    CHECK(picture.analysed() == pristine);

    // And the strong setting lands in the same place however often it is
    // arrived at, which it could not if each pass fed the next.
    REQUIRE(picture.setAdjustments(strong));
    CHECK(picture.refresh());
    CHECK(picture.analysed() == afterMuchDragging);
}

TEST_CASE("A setting that has not changed is not work to do")
{
    LabPicture picture;
    decodeInto(picture, 2, 2);

    ImageAdjustments warm;
    warm.temperature = 0.3f;
    CHECK(picture.setAdjustments(warm));
    CHECK_FALSE(picture.setAdjustments(warm));
    CHECK(picture.refresh());
    CHECK_FALSE(picture.refresh());
}

TEST_CASE("A new photograph keeps the adjustments already set")
{
    // What a visitor comparing two pictures under one adjustment expects.
    LabPicture picture;
    decodeInto(picture, 4, 4);
    ImageAdjustments cool;
    cool.temperature = -0.8f;
    CHECK(picture.setAdjustments(cool));
    CHECK(picture.refresh());

    decodeInto(picture, 6, 2);

    const std::vector<uint8_t>& shown = picture.display().rgba;
    REQUIRE(!shown.empty());
    // Cooling lifts blue over red; the decode had blue above red already, so
    // the gap must have WIDENED rather than stayed where the decode put it.
    CHECK(shown[2] - shown[0] > 170 - 10);
    CHECK(picture.width() == 6);
    CHECK(picture.height() == 2);
}

TEST_CASE("The analysis is offered each new result exactly once")
{
    LabPicture picture;
    CHECK_FALSE(picture.hasFreshPixels());

    decodeInto(picture, 3, 3);
    CHECK(picture.hasFreshPixels());
    picture.pixelsTaken();
    CHECK_FALSE(picture.hasFreshPixels());

    ImageAdjustments lift;
    lift.shadows = 0.5f;
    CHECK(picture.setAdjustments(lift));
    CHECK(picture.refresh());
    CHECK(picture.hasFreshPixels());
}

TEST_CASE("A picture with no size is refused rather than half-adopted")
{
    LabPicture picture;

    CHECK(picture.decodeInto(0, 10) == nullptr);
    CHECK(picture.decodeInto(10, -1) == nullptr);
    CHECK(picture.empty());
    CHECK_FALSE(picture.hasFreshPixels());
}

TEST_CASE("The display carries a new sequence for every result")
{
    // The texture is uploaded on the strength of this number; a result that
    // reused it would leave the canvas showing the previous adjustment.
    LabPicture picture;
    decodeInto(picture, 4, 4);
    const uint64_t afterDecode = picture.display().sequence;

    ImageAdjustments lift;
    lift.exposure = 0.5f;
    CHECK(picture.setAdjustments(lift));
    CHECK(picture.refresh());

    CHECK(picture.display().sequence > afterDecode);
}

}  // namespace sidescopes
