// Native context menu via TrackPopupMenu. Command identifiers are the
// caller's action ids shifted by one, because zero is TrackPopupMenu's
// "dismissed" and the caller's ids start wherever it likes.

#include "platform/native_menu.h"

#include <string>
#include <vector>

#include "platform/windows/wide_strings.h"

namespace sidescopes {

int showNativeContextMenu(const std::vector<NativeMenuItem>& items)
{
    HMENU root = CreatePopupMenu();
    if (!root) {
        return -1;
    }

    std::vector<HMENU> stack{root};
    for (const NativeMenuItem& item : items) {
        HMENU current = stack.back();
        switch (item.kind) {
        case NativeMenuItem::Kind::Separator:
            AppendMenuW(current, MF_SEPARATOR, 0, nullptr);
            break;
        case NativeMenuItem::Kind::SubmenuBegin: {
            HMENU submenu = CreatePopupMenu();
            // On the rare failure the submenu's items land in the
            // enclosing menu - degraded but usable, never a null
            // handle on the stack.
            if (!submenu) {
                break;
            }
            AppendMenuW(current, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), wideFromUtf8(item.label).c_str());
            stack.push_back(submenu);
            break;
        }
        case NativeMenuItem::Kind::SubmenuEnd:
            if (stack.size() > 1) {
                stack.pop_back();
            }
            break;
        case NativeMenuItem::Kind::Action: {
            // A tab column is how Win32 menus draw shortcuts.
            std::string label = item.label;
            if (!item.shortcut.empty()) {
                label += "\t" + item.shortcut;
            }
            AppendMenuW(current, MF_STRING | (item.checked ? MF_CHECKED : MF_UNCHECKED),
                        static_cast<UINT_PTR>(item.actionId + 1), wideFromUtf8(label).c_str());
            break;
        }
        }
    }

    // TrackPopupMenu needs an owner window in the foreground or the menu
    // refuses to dismiss when the user clicks elsewhere. The application
    // window is active - the user just right-clicked it.
    HWND owner = GetActiveWindow();
    if (!owner) {
        owner = GetForegroundWindow();
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    const int command = static_cast<int>(
        TrackPopupMenu(root, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, owner, nullptr));
    DestroyMenu(root);  // recursively destroys the submenus
    return command == 0 ? -1 : command - 1;
}

}  // namespace sidescopes
