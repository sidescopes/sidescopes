#pragma once

#include <string>

namespace sidescopes {

/// The portal request/session object paths, derived purely from the bus name
/// and a per-call token. Kept apart from the D-Bus calls because getting them
/// wrong is silent and total: the portal emits its Response on the path built
/// from OUR unique name, and a listener subscribed to a different path waits
/// forever. Pure and tested so that failure cannot depend on the bus name a
/// particular machine hands out.

/// The sender segment of a request path: the unique bus name with its leading
/// colon dropped and every dot turned to an underscore, exactly as the portal
/// builds it (see the org.freedesktop.portal.Request documentation). Empty in,
/// empty out.
[[nodiscard]] std::string portalSenderToken(const std::string& uniqueBusName);

/// The full request object path the portal will emit a Response on, for a
/// call made under @p uniqueBusName carrying @p handleToken.
[[nodiscard]] std::string portalRequestPath(const std::string& uniqueBusName, const std::string& handleToken);

}  // namespace sidescopes
