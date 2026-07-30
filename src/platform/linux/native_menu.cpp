// The native context menu on Linux, drawn by GTK 3. Unlike an ImGui popup,
// which is clipped to the application's own window, a GTK menu lives in its
// own top-level window: it overflows the small always-on-top window the
// scopes sit in - the whole reason a native menu is wanted here - and wears
// the desktop's theme, so it matches the GTK editors (darktable, RawTherapee)
// the tool is used beside.
//
// GTK is forced to its X11 backend to match the application window, which
// already prefers X11/XWayland: a Wayland GTK popup would have no parent
// surface to attach to. Where no display is reachable at all - a pure-Wayland
// session with no XWayland, or a headless run - GTK cannot initialize, the
// seam reports itself unavailable, and the application draws its ImGui menu
// instead. That fallback is why nativeContextMenuAvailable exists.
//
// The GTK cast macros (GTK_MENU, G_OBJECT, ...) and g_signal_connect expand to
// C-style casts and function-pointer casts that the tidy configuration would
// otherwise reject; they are GTK's own contract for a C API and are wrapped in
// a single NOLINT region rather than sprinkled through the file.

#include "platform/native_menu.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <utility>

namespace sidescopes {
namespace {

// NOLINTBEGIN(cppcoreguidelines-pro-type-cstyle-cast,google-readability-casting,cppcoreguidelines-pro-bounds-array-to-pointer-decay)

/// What an open menu records before its nested loop ends: the activated item's
/// action id, or -1 when the menu was dismissed, plus the loop to quit.
struct MenuResult
{
    int chosen = -1;
    GMainLoop* loop = nullptr;
};

/// Bound to each leaf item's "activate": records the id stashed on the widget.
void onItemActivate(GtkMenuItem* item, gpointer data)
{
    auto* result = static_cast<MenuResult*>(data);
    result->chosen = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "ss-action-id"));
}

/// Bound to the menu's "selection-done", which fires when it closes for any
/// reason - a pick, a click away, Escape, or a failed grab - and ends the
/// modal loop. A pick has already set the id through onItemActivate by then.
void onSelectionDone(GtkMenuShell*, gpointer data)
{
    auto* result = static_cast<MenuResult*>(data);
    if (result->loop != nullptr && g_main_loop_is_running(result->loop)) {
        g_main_loop_quit(result->loop);
    }
}

/// A leaf action: a check item when it carries a checkmark, otherwise a plain
/// one, holding a left label and, when present, a right-aligned dim shortcut
/// hint - the platform menus' own layout.
GtkWidget* makeActionItem(const NativeMenuItem& item, MenuResult* result)
{
    GtkWidget* widget = item.checked ? gtk_check_menu_item_new() : gtk_menu_item_new();
    if (item.checked) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(widget), TRUE);
    }

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget* label = gtk_label_new(item.label.c_str());
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
    if (!item.shortcut.empty()) {
        GtkWidget* accel = gtk_label_new(item.shortcut.c_str());
        gtk_style_context_add_class(gtk_widget_get_style_context(accel), "dim-label");
        gtk_box_pack_end(GTK_BOX(box), accel, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(widget), box);

    g_object_set_data(G_OBJECT(widget), "ss-action-id", GINT_TO_POINTER(item.actionId));
    g_signal_connect(widget, "activate", G_CALLBACK(onItemActivate), result);

    return widget;
}

/// Appends items from @p index into @p shell until the list ends or a
/// SubmenuEnd closes this level; returns the index just past what it consumed.
/// The same flat SubmenuBegin/SubmenuEnd structure the other platforms' menus
/// consume, turned into a GTK submenu tree.
std::size_t buildInto(GtkMenuShell* shell, const std::vector<NativeMenuItem>& items, std::size_t index,
                      MenuResult* result)
{
    while (index < items.size()) {
        const NativeMenuItem& item = items[index];
        switch (item.kind) {
        case NativeMenuItem::Kind::Separator:
            gtk_menu_shell_append(shell, gtk_separator_menu_item_new());
            ++index;
            break;
        case NativeMenuItem::Kind::SubmenuBegin: {
            GtkWidget* parent = gtk_menu_item_new_with_label(item.label.c_str());
            GtkWidget* submenu = gtk_menu_new();
            index = buildInto(GTK_MENU_SHELL(submenu), items, index + 1, result);
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), submenu);
            gtk_menu_shell_append(shell, parent);
            break;
        }
        case NativeMenuItem::Kind::SubmenuEnd:
            return index + 1;
        case NativeMenuItem::Kind::Action:
            gtk_menu_shell_append(shell, makeActionItem(item, result));
            ++index;
            break;
        }
    }

    return index;
}

/// Initializes GTK once, on its X11 backend, and reports whether it took. A
/// missing display leaves it false and the ImGui menu carries the interaction.
bool gtkReady()
{
    static const bool ready = [] {
        if (std::getenv("DISPLAY") == nullptr) {
            return false;
        }
        g_setenv("GDK_BACKEND", "x11", TRUE);

        return gtk_init_check(nullptr, nullptr) == TRUE;
    }();

    return ready;
}

// NOLINTEND(cppcoreguidelines-pro-type-cstyle-cast,google-readability-casting,cppcoreguidelines-pro-bounds-array-to-pointer-decay)

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-pro-type-cstyle-cast,google-readability-casting,cppcoreguidelines-pro-bounds-array-to-pointer-decay)

int showNativeContextMenu(const std::vector<NativeMenuItem>& items)
{
    if (!gtkReady()) {
        return -1;
    }

    MenuResult result;
    GtkWidget* menu = gtk_menu_new();
    g_object_ref_sink(menu);
    buildInto(GTK_MENU_SHELL(menu), items, 0, &result);
    g_signal_connect(menu, "selection-done", G_CALLBACK(onSelectionDone), &result);
    gtk_widget_show_all(menu);

    // The menu is opened from the application's own frame loop, not a GTK
    // event handler, so there is no current event for gtk_menu_popup_at_pointer
    // to anchor and grab from. A button event is synthesized on the root
    // window carrying the live pointer position and device - everything the
    // popup needs to place itself at the cursor and take a valid grab.
    GdkDisplay* display = gdk_display_get_default();
    GdkDevice* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
    GdkWindow* root = gdk_get_default_root_window();
    gint pointerX = 0;
    gint pointerY = 0;
    gdk_device_get_position(pointer, nullptr, &pointerX, &pointerY);

    GdkEvent* trigger = gdk_event_new(GDK_BUTTON_PRESS);
    trigger->button.window = GDK_WINDOW(g_object_ref(root));
    trigger->button.send_event = TRUE;
    trigger->button.time = GDK_CURRENT_TIME;
    trigger->button.x = pointerX;
    trigger->button.y = pointerY;
    trigger->button.x_root = pointerX;
    trigger->button.y_root = pointerY;
    trigger->button.button = GDK_BUTTON_SECONDARY;
    gdk_event_set_device(trigger, pointer);

    // Blocking, like the platform menus this seam serves: the menu runs its own
    // loop until it closes, and the application's frame loop resumes with the
    // choice in hand. selection-done ends the loop even when a grab fails, so a
    // menu that cannot show degrades to a dismissal rather than a hang.
    result.loop = g_main_loop_new(nullptr, FALSE);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), trigger);
    g_main_loop_run(result.loop);
    g_main_loop_unref(result.loop);

    gdk_event_free(trigger);
    gtk_widget_destroy(menu);
    g_object_unref(menu);

    return result.chosen;
}

bool nativeContextMenuAvailable()
{
    return gtkReady();
}

// NOLINTEND(cppcoreguidelines-pro-type-cstyle-cast,google-readability-casting,cppcoreguidelines-pro-bounds-array-to-pointer-decay)

}  // namespace sidescopes
