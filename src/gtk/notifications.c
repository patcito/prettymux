#include "notifications.h"

#include "app_settings.h"
#include "app_support.h"
#include "app_state.h"
#include "shortcuts.h"
#include "workspace.h"

typedef struct {
    char *text;
    int workspace_idx;
    GtkNotebook *pane_notebook;
    int tab_idx;
} NotificationEntry;

typedef struct {
    int workspace_idx;
    GtkNotebook *pane_notebook;
    int tab_idx;
} SidebarToastAction;

typedef struct {
    int workspace_idx;
    GtkNotebook *pane_notebook;
    int tab_idx;
    GtkWidget *popover;
} NotifNavData;

static GPtrArray *g_notifications = NULL;
static SidebarToastAction *g_sidebar_toast_action = NULL;
static guint g_sidebar_toast_timeout_id = 0;
static gboolean g_sidebar_toast_force_bottom = FALSE;
static gboolean g_sidebar_toast_copy_style = FALSE;
static gboolean g_sidebar_toast_recording_marker = FALSE;

static void
notification_entry_free(gpointer data)
{
    NotificationEntry *e = data;
    g_free(e->text);
    g_free(e);
}

static void
sidebar_toast_action_free(SidebarToastAction *action)
{
    if (!action)
        return;
    if (action->pane_notebook)
        g_object_unref(action->pane_notebook);
    g_free(action);
}

void
notifications_init(void)
{
    if (!g_notifications)
        g_notifications = g_ptr_array_new_with_free_func(notification_entry_free);
}

void
notifications_add_full(const char *msg, int ws_idx, GtkNotebook *pane, int tab_idx)
{
    NotificationEntry *e;

    notifications_init();
    e = g_new0(NotificationEntry, 1);
    e->text = g_strdup(msg);
    e->workspace_idx = ws_idx;
    e->pane_notebook = pane;
    e->tab_idx = tab_idx;
    g_ptr_array_add(g_notifications, e);
    while (g_notifications->len > 50)
        g_ptr_array_remove_index(g_notifications, 0);
}

void
notifications_clear(void)
{
    if (g_notifications)
        g_ptr_array_set_size(g_notifications, 0);
}

guint
notifications_count(void)
{
    if (!g_notifications)
        return 0;
    return g_notifications->len;
}

void
bell_button_update(void)
{
    if (!ui.bell_button)
        return;

    guint count = notifications_count();
    if (count > 0) {
        char label[32];
        snprintf(label, sizeof(label), "\360\237\224\224 %u", count);
        gtk_button_set_label(GTK_BUTTON(ui.bell_button), label);
    } else {
        gtk_button_set_label(GTK_BUTTON(ui.bell_button), "\360\237\224\224");
    }
}

void
navigate_to_notification_target(int ws_idx, GtkNotebook *pane_notebook, int tab_idx)
{
    if (ws_idx >= 0 && workspaces && ws_idx < (int)workspaces->len)
        workspace_switch(ws_idx, ui.terminal_stack, ui.workspace_list);

    if (pane_notebook && GTK_IS_NOTEBOOK(pane_notebook) &&
        tab_idx >= 0 &&
        tab_idx < gtk_notebook_get_n_pages(pane_notebook)) {
        gtk_notebook_set_current_page(pane_notebook, tab_idx);
        GhosttyTerminal *term = notebook_terminal_at(pane_notebook, tab_idx);
        if (term)
            ghostty_terminal_focus(term);
    }

    if (g_main_window)
        gtk_window_present(g_main_window);
}

gboolean
notification_target_is_active(int ws_idx, GtkNotebook *pane_notebook, int tab_idx)
{
    Workspace *ws;
    GtkNotebook *focused;
    int current_tab;

    if (!g_main_window_active)
        return FALSE;
    if (ws_idx < 0 || ws_idx != current_workspace || !pane_notebook || tab_idx < 0)
        return FALSE;

    ws = workspace_get_current();
    if (!ws)
        return FALSE;

    focused = workspace_get_focused_pane(ws);
    if (focused != pane_notebook)
        return FALSE;

    current_tab = gtk_notebook_get_current_page(focused);
    return current_tab == tab_idx;
}

void
notifications_apply_toast_position_setting(void)
{
    const char *position;

    if (!ui.toast_revealer)
        return;

    position = g_sidebar_toast_force_bottom
        ? "bottom"
        : app_settings_get_toast_position();
    gtk_widget_set_halign(ui.toast_revealer, GTK_ALIGN_CENTER);

    if (g_strcmp0(position, "bottom") == 0) {
        gtk_widget_set_valign(ui.toast_revealer, GTK_ALIGN_END);
        gtk_widget_set_margin_top(ui.toast_revealer, 12);
        gtk_widget_set_margin_bottom(ui.toast_revealer, 18);
    } else {
        gtk_widget_set_valign(ui.toast_revealer, GTK_ALIGN_START);
        gtk_widget_set_margin_top(ui.toast_revealer, 18);
        gtk_widget_set_margin_bottom(ui.toast_revealer, 12);
    }
}

void
sidebar_toast_hide(void)
{
    if (g_sidebar_toast_timeout_id != 0) {
        g_source_remove(g_sidebar_toast_timeout_id);
        g_sidebar_toast_timeout_id = 0;
    }
    sidebar_toast_action_free(g_sidebar_toast_action);
    g_sidebar_toast_action = NULL;
    g_sidebar_toast_force_bottom = FALSE;
    g_sidebar_toast_copy_style = FALSE;
    g_sidebar_toast_recording_marker = FALSE;
    if (ui.toast_frame)
        gtk_widget_remove_css_class(ui.toast_frame, "copy-toast");
    notifications_apply_toast_position_setting();
    if (ui.toast_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(ui.toast_revealer), FALSE);
}

static gboolean
sidebar_toast_timeout_cb(gpointer user_data)
{
    (void)user_data;
    g_sidebar_toast_timeout_id = 0;
    sidebar_toast_hide();
    return G_SOURCE_REMOVE;
}

static void
on_sidebar_toast_action_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;

    if (!g_sidebar_toast_action)
        return;

    navigate_to_notification_target(g_sidebar_toast_action->workspace_idx,
                                    g_sidebar_toast_action->pane_notebook,
                                    g_sidebar_toast_action->tab_idx);
    sidebar_toast_hide();
}

static void
on_sidebar_toast_close_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    if (g_sidebar_toast_recording_marker) {
        const ShortcutDef *binding = shortcut_find_by_action("recording.mark");
        g_autofree char *keys = shortcut_format_binding(binding);
        shortcut_log_event("recording_start", "recording.mark", keys);
    }
    sidebar_toast_hide();
}

void
sidebar_toast_show_internal(const char *msg, int ws_idx,
                            GtkNotebook *pane_notebook, int tab_idx,
                            gboolean force_bottom,
                            gboolean copy_style,
                            gboolean recording_marker,
                            guint timeout_ms)
{
    if (!ui.toast_revealer || !ui.toast_label || !msg || !msg[0])
        return;

    sidebar_toast_hide();

    g_sidebar_toast_action = g_new0(SidebarToastAction, 1);
    g_sidebar_toast_action->workspace_idx = ws_idx;
    g_sidebar_toast_action->pane_notebook = pane_notebook ? g_object_ref(pane_notebook) : NULL;
    g_sidebar_toast_action->tab_idx = tab_idx;
    g_sidebar_toast_force_bottom = force_bottom;
    g_sidebar_toast_copy_style = copy_style;
    g_sidebar_toast_recording_marker = recording_marker;
    if (ui.toast_frame) {
        if (copy_style)
            gtk_widget_add_css_class(ui.toast_frame, "copy-toast");
        else
            gtk_widget_remove_css_class(ui.toast_frame, "copy-toast");
    }
    notifications_apply_toast_position_setting();

    gtk_label_set_text(GTK_LABEL(ui.toast_label), msg);
    gtk_revealer_set_reveal_child(GTK_REVEALER(ui.toast_revealer), TRUE);
    g_sidebar_toast_timeout_id = g_timeout_add(timeout_ms > 0 ? timeout_ms : 8000,
                                               sidebar_toast_timeout_cb, NULL);
}

void
sidebar_toast_show(const char *msg, int ws_idx, GtkNotebook *pane_notebook, int tab_idx)
{
    sidebar_toast_show_internal(msg, ws_idx, pane_notebook, tab_idx,
                                FALSE, FALSE, FALSE, 8000);
}

void
sidebar_toast_show_copy(const char *msg, int ws_idx,
                        GtkNotebook *pane_notebook, int tab_idx)
{
    sidebar_toast_show_internal(msg, ws_idx, pane_notebook, tab_idx,
                                TRUE, TRUE, FALSE, 2000);
}

static void
on_notif_row_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    NotifNavData *nav = user_data;

    navigate_to_notification_target(nav->workspace_idx,
                                    nav->pane_notebook,
                                    nav->tab_idx);

    if (nav->popover && GTK_IS_POPOVER(nav->popover))
        gtk_popover_popdown(GTK_POPOVER(nav->popover));
}

void
notifications_on_bell_button_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *widget = GTK_WIDGET(btn);
    GtkWidget *popover;
    GtkWidget *vbox;

    (void)user_data;
    notifications_init();

    popover = gtk_popover_new();
    gtk_widget_set_parent(popover, widget);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(vbox, 4);
    gtk_widget_set_margin_end(vbox, 4);
    gtk_widget_set_margin_top(vbox, 4);
    gtk_widget_set_margin_bottom(vbox, 4);
    gtk_widget_set_size_request(vbox, 280, -1);

    if (g_notifications->len == 0) {
        GtkWidget *lbl = gtk_label_new("No notifications");
        gtk_widget_set_margin_top(lbl, 8);
        gtk_widget_set_margin_bottom(lbl, 8);
        gtk_box_append(GTK_BOX(vbox), lbl);
    } else {
        for (guint i = g_notifications->len; i > 0; i--) {
            NotificationEntry *e = g_ptr_array_index(g_notifications, i - 1);
            GtkWidget *row_btn = gtk_button_new_with_label(e->text);
            NotifNavData *nav = g_new0(NotifNavData, 1);

            gtk_widget_set_hexpand(row_btn, TRUE);
            gtk_button_set_has_frame(GTK_BUTTON(row_btn), FALSE);

            nav->workspace_idx = e->workspace_idx;
            nav->pane_notebook = e->pane_notebook;
            nav->tab_idx = e->tab_idx;
            nav->popover = popover;
            g_object_set_data_full(G_OBJECT(row_btn), "notif-nav-data", nav, g_free);

            g_signal_connect(row_btn, "clicked",
                             G_CALLBACK(on_notif_row_clicked), nav);
            gtk_box_append(GTK_BOX(vbox), row_btn);
        }

        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        GtkWidget *clear_btn;

        gtk_widget_set_margin_top(sep, 4);
        gtk_widget_set_margin_bottom(sep, 4);
        gtk_box_append(GTK_BOX(vbox), sep);

        clear_btn = gtk_button_new_with_label("Clear All");
        gtk_widget_set_hexpand(clear_btn, TRUE);
        g_signal_connect_swapped(clear_btn, "clicked",
                                 G_CALLBACK(notifications_clear), NULL);
        g_signal_connect_swapped(clear_btn, "clicked",
                                 G_CALLBACK(bell_button_update), NULL);
        g_signal_connect_swapped(clear_btn, "clicked",
                                 G_CALLBACK(gtk_popover_popdown),
                                 popover);
        gtk_box_append(GTK_BOX(vbox), clear_btn);
    }

    gtk_popover_set_child(GTK_POPOVER(popover), vbox);
    gtk_popover_popup(GTK_POPOVER(popover));
}

void
build_toast_overlay(void)
{
    GtkWidget *toast_box;
    GtkWidget *toast_button;
    GtkWidget *toast_close;

    ui.toast_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(ui.toast_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_reveal_child(GTK_REVEALER(ui.toast_revealer), FALSE);
    gtk_widget_set_margin_start(ui.toast_revealer, 12);
    gtk_widget_set_margin_end(ui.toast_revealer, 12);

    ui.toast_frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(ui.toast_frame, "prettymux-toast");
    gtk_widget_set_size_request(ui.toast_frame, 360, -1);
    gtk_revealer_set_child(GTK_REVEALER(ui.toast_revealer), ui.toast_frame);

    toast_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(toast_box, 10);
    gtk_widget_set_margin_end(toast_box, 10);
    gtk_widget_set_margin_top(toast_box, 8);
    gtk_widget_set_margin_bottom(toast_box, 8);
    gtk_frame_set_child(GTK_FRAME(ui.toast_frame), toast_box);

    toast_button = gtk_button_new();
    gtk_widget_add_css_class(toast_button, "flat");
    gtk_widget_set_hexpand(toast_button, TRUE);
    gtk_widget_set_halign(toast_button, GTK_ALIGN_FILL);
    g_signal_connect(toast_button, "clicked",
                     G_CALLBACK(on_sidebar_toast_action_clicked), NULL);

    ui.toast_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ui.toast_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(ui.toast_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(ui.toast_label), PANGO_WRAP_WORD_CHAR);
    gtk_button_set_child(GTK_BUTTON(toast_button), ui.toast_label);
    gtk_box_append(GTK_BOX(toast_box), toast_button);

    toast_close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(toast_close, "flat");
    g_signal_connect(toast_close, "clicked",
                     G_CALLBACK(on_sidebar_toast_close_clicked), NULL);
    gtk_box_append(GTK_BOX(toast_box), toast_close);

    gtk_overlay_add_overlay(GTK_OVERLAY(ui.overlay), ui.toast_revealer);
    notifications_apply_toast_position_setting();
}
