#include "platform/linux/portal_request_path.h"

namespace sidescopes {

std::string portalSenderToken(const std::string& uniqueBusName)
{
    std::string token = uniqueBusName;
    if (!token.empty() && token.front() == ':') {
        token.erase(token.begin());
    }
    for (char& piece : token) {
        if (piece == '.') {
            piece = '_';
        }
    }
    return token;
}

std::string portalRequestPath(const std::string& uniqueBusName, const std::string& handleToken)
{
    return "/org/freedesktop/portal/desktop/request/" + portalSenderToken(uniqueBusName) + "/" + handleToken;
}

}  // namespace sidescopes
