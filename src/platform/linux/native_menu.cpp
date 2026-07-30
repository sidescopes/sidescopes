// Linux has no native context-menu service to borrow - a menu here would be
// this application's own surface whatever draws it. Until an ImGui-drawn
// fallback exists the menu reports dismissal, which no caller distinguishes
// from the user closing it.

#include "platform/native_menu.h"

namespace sidescopes {

int showNativeContextMenu(const std::vector<NativeMenuItem>&)
{
    return -1;
}

}  // namespace sidescopes
