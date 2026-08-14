// The context menu, answered for a browser: no system menu service exists.
//
// The same answer the Linux port gives, and for the same reason. Where
// `nativeContextMenuAvailable()` is false the application draws the identical
// items with its own interface toolkit, so the menu's CONTENTS live in one
// place and only the presentation differs by platform.

#include "platform/native_menu.h"

#include <vector>

namespace sidescopes {

int showNativeContextMenu(const std::vector<NativeMenuItem>&)
{
    // Dismissal, immediately. Nothing calls this while the query below
    // answers false, and answering "dismissed" is the honest reading of a
    // menu that never appeared.
    return -1;
}

bool nativeContextMenuAvailable()
{
    return false;
}

}  // namespace sidescopes
