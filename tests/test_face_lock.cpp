#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "app/face_lock.h"

using namespace sidescopes;
using Catch::Matchers::WithinAbs;

namespace {

FaceAnchor anchorAt(double x, double y, double width)
{
    return FaceAnchor{x, y, width};
}

FaceLockState foreheadLock()
{
    // A face at 500,300 sized 200, cropped to a forehead-like rectangle.
    return face_lock::makeLock(anchorAt(500.0, 300.0, 200.0), LockRect{450.0, 250.0, 550.0, 350.0});
}

}  // namespace

TEST_CASE("A fresh lock reproduces its crop at the picked anchor")
{
    const auto anchor = anchorAt(500.0, 300.0, 200.0);
    const auto lock = face_lock::makeLock(anchor, LockRect{450.0, 250.0, 550.0, 350.0});

    const auto mapped = face_lock::mapRegion(lock, anchor);
    CHECK_THAT(mapped.left, WithinAbs(450.0, 1e-9));
    CHECK_THAT(mapped.top, WithinAbs(250.0, 1e-9));
    CHECK_THAT(mapped.right, WithinAbs(550.0, 1e-9));
    CHECK_THAT(mapped.bottom, WithinAbs(350.0, 1e-9));
}

TEST_CASE("The crop pans and zooms with the anchor")
{
    // A forehead crop: above the face centre, half its width.
    const auto lock = face_lock::makeLock(anchorAt(500.0, 300.0, 200.0), LockRect{450.0, 180.0, 550.0, 240.0});

    // The photo pans right by 300 and zooms to twice the size.
    const auto mapped = face_lock::mapRegion(lock, anchorAt(800.0, 300.0, 400.0));
    CHECK_THAT(mapped.left, WithinAbs(700.0, 1e-9));
    CHECK_THAT(mapped.top, WithinAbs(60.0, 1e-9));
    CHECK_THAT(mapped.right, WithinAbs(900.0, 1e-9));
    CHECK_THAT(mapped.bottom, WithinAbs(180.0, 1e-9));
}

TEST_CASE("A border edit rebinds the crop from the last anchor")
{
    auto lock = face_lock::makeLock(anchorAt(500.0, 300.0, 200.0), LockRect{400.0, 200.0, 600.0, 400.0});

    face_lock::rebindCrop(lock, LockRect{480.0, 220.0, 520.0, 260.0});

    const auto mapped = face_lock::mapRegion(lock, anchorAt(500.0, 300.0, 200.0));
    CHECK_THAT(mapped.left, WithinAbs(480.0, 1e-9));
    CHECK_THAT(mapped.top, WithinAbs(220.0, 1e-9));
    CHECK_THAT(mapped.right, WithinAbs(520.0, 1e-9));
    CHECK_THAT(mapped.bottom, WithinAbs(260.0, 1e-9));
}

TEST_CASE("A window translation carries the anchors")
{
    auto lock = foreheadLock();

    face_lock::translate(lock, 300.0, 120.0);

    const auto mapped = face_lock::mapRegion(lock, lock.lastAnchor);
    CHECK_THAT(mapped.left, WithinAbs(750.0, 1e-9));
    CHECK_THAT(mapped.top, WithinAbs(370.0, 1e-9));
}

TEST_CASE("Only a side-clipped box is distrusted")
{
    const LockRect bounds{0.0, 0.0, 1000.0, 800.0};

    // Comfortably inside.
    CHECK(face_lock::trustworthyBox(LockRect{400.0, 300.0, 600.0, 500.0}, bounds));
    // Flush against the left or right edge: the width lies.
    CHECK_FALSE(face_lock::trustworthyBox(LockRect{0.0, 300.0, 200.0, 500.0}, bounds));
    CHECK_FALSE(face_lock::trustworthyBox(LockRect{802.0, 300.0, 998.0, 500.0}, bounds));
    // Flush against the bottom: the width is intact - a face near the
    // window's bottom edge stays trackable.
    CHECK(face_lock::trustworthyBox(LockRect{400.0, 610.0, 600.0, 800.0}, bounds));
    // A portrait filling the window: near every edge, still trusted.
    CHECK(face_lock::trustworthyBox(LockRect{10.0, 5.0, 990.0, 795.0}, bounds));
}
