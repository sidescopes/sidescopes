#pragma once

#include <vector>

#include "platform/native_menu.h"

namespace sidescopes {

/// One frame of the drawn context menu, where the native one is unavailable.
struct ImGuiContextMenuFrame
{
    /// The popup is on screen; keep feeding it frames.
    bool open = false;
    /// The chosen action id, valid the frame a choice lands; -1 otherwise.
    int chosen = -1;
};

/// Opens the fallback menu popup at the cursor next frame, taking the items
/// to draw until a choice or a dismissal closes it.
void openImGuiContextMenu(std::vector<NativeMenuItem> items);

/// Draws the fallback menu and reports its state. Call every frame from the
/// host window's scope; a frame without an open popup costs nothing.
ImGuiContextMenuFrame drawImGuiContextMenu();

}  // namespace sidescopes
