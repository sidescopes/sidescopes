// The drawn context menu: the fallback shown when no native menu is
// available. macOS and Windows always have one; Linux draws a GTK menu where
// a display is reachable and falls back here where GTK cannot start (a
// headless run, or a Wayland session with no XWayland). The same declarative
// items the native menus consume are rendered as an ImGui popup:
// submenus nest through BeginMenu, checked actions wear a checkmark, and the
// display-only shortcut column teaches the keys exactly as the native menus
// do. Closing the popup without a choice is a dismissal, the same -1 the
// native path reports.

#include "app/imgui_context_menu.h"

#include "imgui.h"

namespace sidescopes {
namespace {

constexpr const char* PopupId = "sidescopes-context-menu";

/// The items the open popup draws. Owned here so the host carries no menu
/// state; the popup's own lifetime is already ImGui-global.
std::vector<NativeMenuItem>& pendingItems()
{
    static std::vector<NativeMenuItem> items;
    return items;
}

/// Draws items from @p index until the end or the submenu close that matches
/// one open. Returns the index past what it consumed; @p chosen carries the
/// clicked action id, if any.
std::size_t drawItems(const std::vector<NativeMenuItem>& items, std::size_t index, int& chosen)
{
    while (index < items.size()) {
        const NativeMenuItem& item = items[index];
        switch (item.kind) {
        case NativeMenuItem::Kind::Separator:
            ImGui::Separator();
            ++index;
            break;
        case NativeMenuItem::Kind::SubmenuBegin:
            if (ImGui::BeginMenu(item.label.c_str())) {
                index = drawItems(items, index + 1, chosen);
                ImGui::EndMenu();
            } else {
                // Skip the closed submenu's items, honoring nesting.
                int depth = 1;
                ++index;
                while (index < items.size() && depth > 0) {
                    if (items[index].kind == NativeMenuItem::Kind::SubmenuBegin) {
                        ++depth;
                    } else if (items[index].kind == NativeMenuItem::Kind::SubmenuEnd) {
                        --depth;
                    }
                    ++index;
                }
            }
            break;
        case NativeMenuItem::Kind::SubmenuEnd:
            return index + 1;
        case NativeMenuItem::Kind::Action:
            if (ImGui::MenuItem(item.label.c_str(), item.shortcut.empty() ? nullptr : item.shortcut.c_str(),
                                item.checked)) {
                chosen = item.actionId;
            }
            ++index;
            break;
        }
    }
    return index;
}

}  // namespace

void openImGuiContextMenu(std::vector<NativeMenuItem> items)
{
    pendingItems() = std::move(items);
    ImGui::OpenPopup(PopupId);
}

ImGuiContextMenuFrame drawImGuiContextMenu()
{
    ImGuiContextMenuFrame frame;
    if (pendingItems().empty()) {
        return frame;
    }
    if (!ImGui::BeginPopup(PopupId)) {
        pendingItems().clear();
        return frame;
    }
    frame.open = true;
    drawItems(pendingItems(), 0, frame.chosen);
    if (frame.chosen >= 0) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return frame;
}

}  // namespace sidescopes
