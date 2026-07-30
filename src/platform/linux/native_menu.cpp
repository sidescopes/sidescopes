// Linux has no native context-menu service to borrow, so the application
// draws the same declarative items itself with ImGui; this seam only has to
// say so. The blocking entry point stays for the contract and reports
// dismissal if anything calls it anyway.

#include "platform/native_menu.h"

namespace sidescopes {

int showNativeContextMenu(const std::vector<NativeMenuItem>&)
{
    return -1;
}

bool nativeContextMenuAvailable()
{
    return false;
}

}  // namespace sidescopes
