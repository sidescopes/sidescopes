#include <catch2/catch_test_macros.hpp>

#include "platform/linux/linux_session.h"

using namespace sidescopes;

// The session-type decision picks the whole capture backend: XShm on X11, the
// portal on Wayland. WAYLAND_DISPLAY is the deciding signal, because XWayland
// sets DISPLAY too - reading DISPLAY alone would send a Wayland session down
// the X11 path, where XShm sees only X clients and never the real screen.

TEST_CASE("a Wayland display is not an X11 session, even with DISPLAY also set")
{
    CHECK_FALSE(isX11Session("wayland-0", ":0"));
    CHECK_FALSE(isX11Session("wayland-0", nullptr));
}

TEST_CASE("no Wayland display with a usable DISPLAY is an X11 session")
{
    CHECK(isX11Session(nullptr, ":0"));
    // An empty variable is a cleared one, the same as unset.
    CHECK(isX11Session("", ":0"));
}

TEST_CASE("no usable display at all is not an X11 session")
{
    CHECK_FALSE(isX11Session(nullptr, nullptr));
    CHECK_FALSE(isX11Session(nullptr, ""));
    CHECK_FALSE(isX11Session("", ""));
}
