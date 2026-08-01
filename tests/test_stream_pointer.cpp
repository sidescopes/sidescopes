// On a Wayland session the compositor is the only thing that knows where the
// pointer is: X is told only while it sits over an X surface, measured here as
// 450 polls at one unchanging coordinate through a minute of real movement over
// a Firefox window. The position therefore arrives with the capture stream, in
// FRAME pixels, and has to leave as a desktop point - and both the conversion
// and the mode that makes it arrive at all fail silently, showing a colour that
// simply never changes.

#include <catch2/catch_test_macros.hpp>

#include "platform/linux/portal_options.h"
#include "platform/linux/stream_pointer.h"

using namespace sidescopes;

namespace {

StreamPlacement placementOf(double originX, double originY, double widthPoints, double heightPoints, int frameWidth,
                            int frameHeight)
{
    StreamPlacement placement;
    placement.originX = originX;
    placement.originY = originY;
    placement.widthPoints = widthPoints;
    placement.heightPoints = heightPoints;
    placement.frameWidth = frameWidth;
    placement.frameHeight = frameHeight;

    return placement;
}

}  // namespace

TEST_CASE("a stream point on an unscaled output is its own desktop point")
{
    const StreamPlacement placement = placementOf(0.0, 0.0, 1920.0, 1080.0, 1920, 1080);
    const std::optional<DesktopPoint> point = streamPointToDesktop(placement, 640, 360);
    REQUIRE(point.has_value());
    CHECK(point->x == 640.0);
    CHECK(point->y == 360.0);
}

TEST_CASE("a stream point carries its output's origin onto the desktop")
{
    // The second monitor of a side-by-side pair: the same frame pixel is a
    // different desktop point, and dropping the origin puts every probe on the
    // wrong screen.
    const StreamPlacement placement = placementOf(1920.0, 0.0, 1920.0, 1080.0, 1920, 1080);
    const std::optional<DesktopPoint> point = streamPointToDesktop(placement, 10, 20);
    REQUIRE(point.has_value());
    CHECK(point->x == 1930.0);
    CHECK(point->y == 20.0);
}

TEST_CASE("a scaled output converts frame pixels into desktop points")
{
    // A 3840x2160 frame covering a 1920x1080 logical output: two frame pixels
    // to the point. Reading the frame's own numbers would put the probe at
    // twice its distance from the origin.
    const StreamPlacement placement = placementOf(100.0, 50.0, 1920.0, 1080.0, 3840, 2160);
    const std::optional<DesktopPoint> point = streamPointToDesktop(placement, 800, 400);
    REQUIRE(point.has_value());
    CHECK(point->x == 500.0);
    CHECK(point->y == 250.0);
}

TEST_CASE("a portal that states no extent lets the frame speak for itself")
{
    // position and size are optional in the portal spec. Absent, the frame is
    // its own space - right for every unscaled single-output session, which is
    // the common case this must not break.
    const StreamPlacement placement = placementOf(0.0, 0.0, 0.0, 0.0, 1600, 900);
    const std::optional<DesktopPoint> point = streamPointToDesktop(placement, 400, 300);
    REQUIRE(point.has_value());
    CHECK(point->x == 400.0);
    CHECK(point->y == 300.0);
}

TEST_CASE("a cursor outside the frame reports no position at all")
{
    // A compositor states the cursor's position only while it is over the
    // captured output. Clamping to the nearest edge would pin the probe to the
    // border and read as live-but-wrong; nothing sends it to the fallback.
    const StreamPlacement placement = placementOf(0.0, 0.0, 1920.0, 1080.0, 1920, 1080);
    CHECK_FALSE(streamPointToDesktop(placement, -1, 500).has_value());
    CHECK_FALSE(streamPointToDesktop(placement, 500, -1).has_value());
    CHECK_FALSE(streamPointToDesktop(placement, 1920, 500).has_value());
    CHECK_FALSE(streamPointToDesktop(placement, 500, 1080).has_value());
}

TEST_CASE("a stream with no frame size yet reports no position")
{
    const StreamPlacement placement = placementOf(0.0, 0.0, 1920.0, 1080.0, 0, 0);
    CHECK_FALSE(streamPointToDesktop(placement, 0, 0).has_value());
}

TEST_CASE("the published pointer survives until it is cleared")
{
    publishStreamPointer(DesktopPoint{12.0, 34.0});
    const std::optional<DesktopPoint> held = streamPointer();
    REQUIRE(held.has_value());
    CHECK(held->x == 12.0);
    CHECK(held->y == 34.0);

    // Cleared rather than aged out: a compositor sends a frame for cursor
    // motion as well as for damage, so silence means the pointer stopped and
    // the last position is still where it is. Only stream stop makes it a lie.
    publishStreamPointer(std::nullopt);
    CHECK_FALSE(streamPointer().has_value());
}

TEST_CASE("cursor metadata is asked for exactly when the portal offers it")
{
    constexpr uint32_t Hidden = 1;
    constexpr uint32_t Embedded = 2;
    constexpr uint32_t Metadata = 4;

    // Asking for a mode the portal does not advertise is an error that fails
    // the whole handshake, so a portal without metadata must be asked for
    // hidden - losing the probe's pointer, never the capture itself.
    CHECK(portalCursorMode(Hidden) == Hidden);
    CHECK(portalCursorMode(Hidden | Embedded) == Hidden);
    CHECK(portalCursorMode(0) == Hidden);

    // Embedded is never chosen even when offered: a cursor drawn into the
    // pixels would enter the scopes' own analysis.
    CHECK(portalCursorMode(Hidden | Embedded | Metadata) == Metadata);
    CHECK(portalCursorMode(Metadata) == Metadata);
}

TEST_CASE("the cursor metadata range covers what a compositor asks for")
{
    // MEASURED, and it is why the live probe was dead on Wayland: GNOME's
    // compositor negotiates a 384x384 cursor bitmap - 589872 bytes, read
    // straight off a running stream. A consumer whose declared range stops
    // below that does not receive a smaller block. The ranges fail to
    // intersect, the metadata is dropped from the buffer with no error
    // anywhere, and the only symptom is a pointer position that never arrives.
    //
    // This constant stood at 256 and the probe simply never moved. Raising it
    // is safe; lowering it is the bug, which is what this test exists to say.
    CHECK(CursorBitmapSide >= 384);
}

TEST_CASE("a source kind asks the compositor for that kind and no other")
{
    // Claimed to be pinned by a test since it was written, and never was.
    CHECK(portalSourceTypeMask(PortalSourceKind::Monitor) == 1u);
    CHECK(portalSourceTypeMask(PortalSourceKind::Window) == 2u);
}
