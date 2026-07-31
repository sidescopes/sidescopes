#include "platform/linux/linux_session.h"

#include <cstdlib>

namespace sidescopes {

bool isX11Session(const char* waylandDisplay, const char* xDisplay)
{
    const bool wayland = waylandDisplay != nullptr && waylandDisplay[0] != '\0';
    if (wayland) {
        return false;
    }
    return xDisplay != nullptr && xDisplay[0] != '\0';
}

bool runningOnX11Session()
{
    return isX11Session(std::getenv("WAYLAND_DISPLAY"), std::getenv("DISPLAY"));
}

}  // namespace sidescopes
