// The portal request-path builder decides whether the ScreenCast handshake
// completes at all: the portal emits its Response on the object path derived
// from this connection's unique bus name, and a listener subscribed to any
// other path waits forever with no error. The bug is silent and total, and it
// depends on the shape of a bus name the machine hands out - so the exact
// transformation is pinned here across the range of names D-Bus produces.

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "platform/linux/portal_request_path.h"

using sidescopes::portalRequestPath;
using sidescopes::portalSenderToken;

TEST_CASE("the sender token drops the leading colon and escapes dots")
{
    // A real unique name: ":1.42" becomes "1_42", which is exactly the segment
    // the portal builds the request path from.
    CHECK(portalSenderToken(":1.42") == "1_42");
    CHECK(portalSenderToken(":1.2340") == "1_2340");
}

TEST_CASE("every dot is escaped, not only the first")
{
    // Unique names are two components today, but the escaping is defined per
    // character; a name with more dots must escape all of them or the path
    // diverges past the first.
    CHECK(portalSenderToken(":1.2.3") == "1_2_3");
}

TEST_CASE("only a leading colon is dropped")
{
    // The colon is a prefix marker on unique names; one anywhere else is not
    // special and must survive, since the portal's own escaping leaves it.
    CHECK(portalSenderToken("1.2") == "1_2");
    CHECK(portalSenderToken("") == "");
}

TEST_CASE("the request path joins the sender token and the handle token")
{
    CHECK(portalRequestPath(":1.42", "sidescopes1") == "/org/freedesktop/portal/desktop/request/1_42/sidescopes1");
}

TEST_CASE("the handle token is placed verbatim")
{
    // The handle token is ours to choose and carries no dots; it must reach the
    // path unaltered, since it is what the SelectSources/Start options declared.
    CHECK(portalRequestPath(":1.7", "sidescopes3") == "/org/freedesktop/portal/desktop/request/1_7/sidescopes3");
}
