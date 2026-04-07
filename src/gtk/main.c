#define _GNU_SOURCE
// PrettyMux — GTK4 + WebKitGTK + ghostty (OpenGL)
// GPU-accelerated terminal multiplexer with integrated browser

#include <adwaita.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#include <glib-unix.h>
#endif

#include "ghostty.h"
#include "ghostty_terminal.h"
#include "browser_tab.h"
#include "app_settings.h"
#include "hover_focus.h"
#include "theme.h"
#include "shortcuts.h"
#include "workspace.h"
#include "session.h"
#include "close_confirm.h"
#include "command_palette.h"
#include "port_scanner.h"
#include "socket_server.h"
#include "shortcuts_overlay.h"
#include "settings_dialog.h"
#include "pane_move_overlay.h"
#include "pip_window.h"
#include "resize_overlay.h"
#include "prettymux_agent_cli.h"

// ── Global state ──

ghostty_app_t g_ghostty_app = NULL;
float g_ghostty_default_font_size = 0.0f;
static GtkWindow *g_main_window = NULL;
static gboolean g_main_window_active = FALSE;
static gboolean g_app_quit_in_progress = FALSE;
static ghostty_config_t g_runtime_ghostty_config = NULL;
static guint g_desktop_notification_serial = 0;
static gboolean g_sidebar_toast_force_bottom = FALSE;
static gboolean g_sidebar_toast_copy_style = FALSE;
static gboolean g_sidebar_toast_recording_marker = FALSE;
static gint64 g_recording_origin_us = -1;
static GhosttyTerminal *g_terminal_search_target = NULL;
static gint64 g_terminal_search_total = -1;
static gint64 g_terminal_search_selected = -1;
static GtkWindow *g_about_window = NULL;

#ifndef PRETTYMUX_VERSION
#define PRETTYMUX_VERSION "0.2.9"
#endif

#define PRETTYMUX_GITHUB_URL "https://github.com/patcito/prettymux"
#define PRETTYMUX_WEBSITE_URL "https://prettymux.com/"
#define PRETTYMUX_LICENSE_NAME "GPL-3.0-only"

static void debug_notification_log(const char *fmt, ...);
static void apply_toast_position_setting(void);
static void handle_reported_port(const char *terminal_id, int port);
static void terminal_search_send_action(GhosttyTerminal *term, const char *action);
static void terminal_search_hide(void);
static void about_dialog_present(GtkWindow *parent);

// Widget references for the main window layout
static struct {
    GtkWidget *outer_paned;
    GtkWidget *sidebar_box;
    GtkWidget *workspace_list;
    GtkWidget *main_paned;
    GtkWidget *terminal_box;      // GtkBox holding terminal_stack + notes panel
    GtkWidget *terminal_stack;
    GtkWidget *browser_notebook;
    GtkWidget *overlay;           // GtkOverlay wrapping the outer_paned
    GtkWidget *command_palette;   // CommandPalette overlay widget
    GtkWidget *bell_button;       // Bell/notification button in sidebar header
    GtkWidget *toast_revealer;
    GtkWidget *toast_frame;
    GtkWidget *toast_label;
} ui = {0};

void
shortcut_log_event(const char *type, const char *action, const char *keys)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(GDateTime) now = NULL;
    g_autofree char *path = NULL;
    g_autofree char *dir = NULL;
    g_autofree char *timestamp = NULL;
    g_autofree char *json = NULL;
    gsize json_len = 0;
    FILE *fp;
    gint64 now_us = g_get_monotonic_time();
    gint64 relative_ms;
    Workspace *ws = workspace_get_current();
    GtkNotebook *pane = ws ? workspace_get_focused_pane(ws) : NULL;
    int pane_idx = (ws && pane) ? workspace_get_pane_index(ws, pane) : -1;
    int tab_idx = (pane && GTK_IS_NOTEBOOK(pane))
        ? gtk_notebook_get_current_page(pane) : -1;

    if (g_strcmp0(type, "recording_start") == 0) {
        g_recording_origin_us = now_us;
        relative_ms = 0;
    } else if (g_recording_origin_us > 0) {
        relative_ms = (now_us - g_recording_origin_us) / 1000;
    } else {
        relative_ms = now_us / 1000;
    }

    path = g_build_filename(g_get_home_dir(), ".local", "state",
                            "prettymux", "shortcuts.jsonl", NULL);
    dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0755) != 0)
        return;

    now = g_date_time_new_now_local();
    timestamp = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S.%f%z");

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, type ? type : "shortcut");
    json_builder_set_member_name(builder, "ts");
    json_builder_add_string_value(builder, timestamp ? timestamp : "");
    json_builder_set_member_name(builder, "t_ms");
    json_builder_add_int_value(builder, relative_ms);
    json_builder_set_member_name(builder, "mono_ms");
    json_builder_add_int_value(builder, now_us / 1000);
    json_builder_set_member_name(builder, "action");
    json_builder_add_string_value(builder, action ? action : "");
    json_builder_set_member_name(builder, "keys");
    json_builder_add_string_value(builder, keys ? keys : "");
    json_builder_set_member_name(builder, "workspace");
    json_builder_add_int_value(builder, current_workspace);
    json_builder_set_member_name(builder, "pane");
    json_builder_add_int_value(builder, pane_idx);
    json_builder_set_member_name(builder, "tab");
    json_builder_add_int_value(builder, tab_idx);
    json_builder_end_object(builder);

    generator = json_generator_new();
    {
        g_autoptr(JsonNode) root = json_builder_get_root(builder);
        json_generator_set_root(generator, root);
        json = json_generator_to_data(generator, &json_len);
    }

    fp = fopen(path, "a");
    if (!fp)
        return;
    fwrite(json, 1, json_len, fp);
    fputc('\n', fp);
    fclose(fp);
}

static void
on_main_window_active_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    (void)user_data;
    g_main_window_active = gtk_window_is_active(GTK_WINDOW(object));
    hover_focus_handle_window_active_changed(g_main_window_active);
    port_scanner_set_window_active(g_main_window_active);
}

static void
apply_theme_to_ghostty_scheme(void)
{
    ghostty_color_scheme_e scheme;
    const Theme *t;

    if (!g_ghostty_app)
        return;

    t = theme_get_current();
    scheme = (strcmp(t->name, "Light") == 0)
        ? GHOSTTY_COLOR_SCHEME_LIGHT
        : GHOSTTY_COLOR_SCHEME_DARK;
    ghostty_app_set_color_scheme(g_ghostty_app, scheme);

    if (!workspaces)
        return;

    for (guint wi = 0; wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);
        if (!ws->terminals)
            continue;
        for (guint ti = 0; ti < ws->terminals->len; ti++) {
            GhosttyTerminal *term = g_ptr_array_index(ws->terminals, ti);
            ghostty_surface_t surf = ghostty_terminal_get_surface(term);
            if (surf)
                ghostty_surface_set_color_scheme(surf, scheme);
        }
    }
}

static void
sync_ghostty_theme_to_prettymux_theme(void)
{
    const Theme *t = theme_get_current();

    app_settings_set_ghostty_theme(
        app_settings_default_ghostty_theme_for_prettymux_theme(t ? t->name : NULL));
}

static ghostty_config_t
load_ghostty_config_with_overrides(void)
{
    ghostty_config_t config = ghostty_config_new();
    char *override_path = NULL;
    double font_size = 0.0;
    const char *key = "font-size";

    ghostty_config_load_default_files(config);
    override_path = app_settings_ghostty_override_path();
    if (g_file_test(override_path, G_FILE_TEST_EXISTS))
        ghostty_config_load_file(config, override_path);
    g_free(override_path);

    g_ghostty_default_font_size = 0.0f;
    if (ghostty_config_get(config, &font_size, key, strlen(key)) &&
        font_size > 0.0)
        g_ghostty_default_font_size = (float)font_size;

    ghostty_config_finalize(config);
    return config;
}

static void
apply_runtime_settings(void *user_data)
{
    (void)user_data;

    app_settings_write_ghostty_override();
    theme_set_custom(app_settings_get_custom_theme());

    if (g_ghostty_app) {
        g_runtime_ghostty_config = load_ghostty_config_with_overrides();
        ghostty_app_update_config(g_ghostty_app, g_runtime_ghostty_config);

        if (workspaces) {
            for (guint wi = 0; wi < workspaces->len; wi++) {
                Workspace *ws = g_ptr_array_index(workspaces, wi);
                if (!ws->terminals)
                    continue;
                for (guint ti = 0; ti < ws->terminals->len; ti++) {
                    GhosttyTerminal *term = g_ptr_array_index(ws->terminals, ti);
                    ghostty_surface_t surf = ghostty_terminal_get_surface(term);
                    if (surf)
                        ghostty_surface_update_config(surf, g_runtime_ghostty_config);
                }
            }
        }
    }

    apply_theme_to_ghostty_scheme();
    apply_toast_position_setting();
    session_queue_save();
}

static void
apply_toast_position_setting(void)
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

// ── Notification system ──

typedef struct {
    char *text;
    int   workspace_idx;
    GtkNotebook *pane_notebook;   /* may become stale; checked before use */
    int   tab_idx;
} NotificationEntry;

typedef struct {
    int workspace_idx;
    GtkNotebook *pane_notebook;
    int tab_idx;
} SidebarToastAction;

static void notification_entry_free(gpointer data) {
    NotificationEntry *e = data;
    g_free(e->text);
    g_free(e);
}

static GPtrArray *g_notifications = NULL;
static SidebarToastAction *g_sidebar_toast_action = NULL;
static guint g_sidebar_toast_timeout_id = 0;

static void
sidebar_toast_action_free(SidebarToastAction *action)
{
    if (!action)
        return;
    if (action->pane_notebook)
        g_object_unref(action->pane_notebook);
    g_free(action);
}

static GtkWidget *
page_linked_terminal(GtkWidget *page)
{
    GtkWidget *terminal;

    if (!page)
        return NULL;
    if (GHOSTTY_IS_TERMINAL(page))
        return page;

    terminal = g_object_get_data(G_OBJECT(page), "linked-terminal");
    return (terminal && GHOSTTY_IS_TERMINAL(terminal)) ? terminal : NULL;
}

static GhosttyTerminal *
notebook_terminal_at(GtkNotebook *notebook, int page_num)
{
    GtkWidget *terminal = page_linked_terminal(
        gtk_notebook_get_nth_page(notebook, page_num));
    return terminal ? GHOSTTY_TERMINAL(terminal) : NULL;
}

static gboolean
focus_within_terminal(GhosttyTerminal *term)
{
    GtkWidget *focus;

    if (!term || !g_main_window)
        return FALSE;

    focus = gtk_window_get_focus(GTK_WINDOW(g_main_window));
    return focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(term));
}

static gboolean
is_modifier_key(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R:
    case GDK_KEY_Hyper_L:
    case GDK_KEY_Hyper_R:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean
terminal_search_handle_key(guint keyval, GdkModifierType state)
{
    GdkModifierType mods;
    const char *old_query;
    g_autofree char *new_query = NULL;

    if (!g_terminal_search_target ||
        !ghostty_terminal_is_search_active(g_terminal_search_target))
        return FALSE;

    mods = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                    GDK_ALT_MASK | GDK_SUPER_MASK);

    if (keyval == GDK_KEY_Escape) {
        terminal_search_send_action(g_terminal_search_target, "end_search");
        terminal_search_hide();
        return TRUE;
    }

    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
         keyval == GDK_KEY_ISO_Enter) &&
        (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0) {
        terminal_search_send_action(g_terminal_search_target,
                                    (mods & GDK_SHIFT_MASK)
                                        ? "navigate_search:previous"
                                        : "navigate_search:next");
        return TRUE;
    }

    old_query = ghostty_terminal_get_search_query(g_terminal_search_target);

    if (keyval == GDK_KEY_BackSpace &&
        (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0) {
        const char *prev = g_utf8_find_prev_char(old_query, old_query + strlen(old_query));
        new_query = prev ? g_strndup(old_query, prev - old_query) : g_strdup("");
    } else if ((mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0) {
        gunichar uc = gdk_keyval_to_unicode(keyval);
        if (uc >= 0x20 && uc != 0) {
            char buf[8] = {0};
            int len = g_unichar_to_utf8(uc, buf);
            buf[len] = '\0';
            new_query = g_strdup_printf("%s%s", old_query ? old_query : "", buf);
        }
    }

    if (!new_query)
        return FALSE;

    ghostty_terminal_set_search_active(g_terminal_search_target, TRUE, new_query);
    {
        g_autofree char *action = g_strdup_printf("search:%s", new_query);
        terminal_search_send_action(g_terminal_search_target, action);
    }
    return TRUE;
}

static void
broadcast_key_to_workspace(Workspace *ws,
                           GhosttyTerminal *source,
                           guint keyval,
                           guint keycode,
                           GdkModifierType state)
{
    if (!ws || !source || !ws->terminals)
        return;

    for (guint i = 0; i < ws->terminals->len; i++) {
        GhosttyTerminal *term = g_ptr_array_index(ws->terminals, i);
        if (!term || term == source)
            continue;
        ghostty_terminal_send_key_event(term, keyval, keycode, state);
    }
}

static char *
prettymux_trim_last_lines(const char *text, int lines)
{
    const char *start;
    const char *cursor;

    if (!text)
        return g_strdup("");
    if (lines <= 0)
        return g_strdup(text);

    start = text;
    cursor = text + strlen(text);
    while (cursor > start && lines > 0) {
        cursor--;
        if (*cursor == '\n' && cursor + 1 < text + strlen(text))
            lines--;
    }

    if (lines <= 0 && cursor > start)
        cursor++;
    else
        cursor = start;

    return g_strdup(cursor);
}

static const char *
prettymux_icon_name(void)
{
    return "prettymux";
}

static void
ensure_local_desktop_entry(void)
{
#ifdef G_OS_WIN32
    return;
#else
    const char *app_id = "dev.prettymux.app";
    g_autofree char *desktop_dir = NULL;
    g_autofree char *desktop_path = NULL;
    g_autofree char *exe_path = NULL;
    g_autofree char *icon_path = NULL;
    g_autofree char *icon_path_real = NULL;
    g_autofree char *exe_path_real = NULL;
    g_autofree char *desktop_contents = NULL;
    char exe_buf[PATH_MAX];
    ssize_t exe_len;

    if (g_file_test("/usr/share/applications/dev.prettymux.app.desktop",
                    G_FILE_TEST_EXISTS) ||
        g_file_test("/app/share/applications/dev.prettymux.app.desktop",
                    G_FILE_TEST_EXISTS)) {
        return;
    }

    desktop_dir = g_build_filename(g_get_home_dir(), ".local", "share",
                                   "applications", NULL);
    desktop_path = g_build_filename(desktop_dir, "dev.prettymux.app.desktop", NULL);

    exe_len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (exe_len <= 0)
        return;
    exe_buf[exe_len] = '\0';
    exe_path = g_strdup(exe_buf);
    exe_path_real = g_canonicalize_filename(exe_path, NULL);

    icon_path = g_build_filename(PRETTYMUX_SOURCE_DIR, "..", "..",
                                 "packaging", "prettymux.svg", NULL);
    if (!g_file_test(icon_path, G_FILE_TEST_EXISTS)) {
        g_free(icon_path);
        icon_path = g_strdup("prettymux");
    } else {
        icon_path_real = g_canonicalize_filename(icon_path, NULL);
    }

    if (g_mkdir_with_parents(desktop_dir, 0755) != 0)
        return;

    desktop_contents = g_strdup_printf(
        "[Desktop Entry]\n"
        "Name=PrettyMux\n"
        "Comment=GPU-accelerated terminal multiplexer\n"
        "Exec=%s\n"
        "Icon=%s\n"
        "Type=Application\n"
        "Categories=System;TerminalEmulator;\n"
        "Keywords=terminal;multiplexer;gpu;\n"
        "StartupNotify=true\n"
        "DBusActivatable=true\n"
        "X-GNOME-UsesNotifications=true\n"
        "X-Flatpak=%s\n",
        exe_path_real ? exe_path_real : exe_path,
        icon_path_real ? icon_path_real : icon_path,
        app_id);

    if (!g_file_set_contents(desktop_path, desktop_contents, -1, NULL))
        return;

    debug_notification_log("notify desktop-entry ensured path=%s exec=%s icon=%s",
                           desktop_path, exe_path, icon_path);
#endif
}

static void
debug_notification_log(const char *fmt, ...)
{
    va_list ap;
    g_autofree char *msg = NULL;
    FILE *fp;

    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    g_message("%s", msg);

    fp = fopen("/tmp/prettymux-notify-debug.log", "a");
    if (!fp)
        return;
    fprintf(fp, "%s\n", msg);
    fclose(fp);
}

static void
send_desktop_notification(const char *title, const char *body,
                          int ws_idx, int pane_idx, int tab_idx)
{
    GApplication *app = g_application_get_default();
    GNotification *notification;
    GIcon *icon;
    gchar *id;

    if (!app) {
        debug_notification_log("notify skip no-app title=%s body=%s",
                               title ? title : "",
                               body ? body : "");
        return;
    }

    debug_notification_log(
        "notify send begin id_next=%u app_id=%s registered=%d dbus=%p active=%d target=(%d,%d,%d) title=%s body=%s",
        g_desktop_notification_serial + 1,
        g_application_get_application_id(app)
            ? g_application_get_application_id(app) : "(null)",
        g_application_get_is_registered(app),
        g_application_get_dbus_connection(app),
        g_main_window_active,
        ws_idx, pane_idx, tab_idx,
        title ? title : "",
        body ? body : "");

    notification = g_notification_new(title ? title : "PrettyMux");
    if (body && body[0])
        g_notification_set_body(notification, body);
    g_notification_set_priority(notification, G_NOTIFICATION_PRIORITY_NORMAL);

    icon = g_themed_icon_new(prettymux_icon_name());
    g_notification_set_icon(notification, icon);
    g_object_unref(icon);

    if (ws_idx >= 0 && pane_idx >= 0 && tab_idx >= 0) {
        g_notification_set_default_action_and_target_value(
            notification,
            "app.navigate-to-terminal",
            g_variant_new("(iii)", ws_idx, pane_idx, tab_idx));
    }

    id = g_strdup_printf("desktop-%u", ++g_desktop_notification_serial);
    g_application_send_notification(app, id, notification);
    debug_notification_log("notify send done id=%s", id);
    g_free(id);
    g_object_unref(notification);
}

static void notifications_init(void) {
    if (!g_notifications)
        g_notifications = g_ptr_array_new_with_free_func(notification_entry_free);
}

static void notifications_add_full(const char *msg, int ws_idx,
                                   GtkNotebook *pane, int tab_idx) {
    notifications_init();
    NotificationEntry *e = g_new0(NotificationEntry, 1);
    e->text = g_strdup(msg);
    e->workspace_idx = ws_idx;
    e->pane_notebook = pane;
    e->tab_idx = tab_idx;
    g_ptr_array_add(g_notifications, e);
    /* Cap at 50 */
    while (g_notifications->len > 50)
        g_ptr_array_remove_index(g_notifications, 0);
}

static void notifications_clear(void) {
    if (g_notifications)
        g_ptr_array_set_size(g_notifications, 0);
}

static guint notifications_count(void) {
    if (!g_notifications) return 0;
    return g_notifications->len;
}

static void bell_button_update(void) {
    if (!ui.bell_button) return;
    guint count = notifications_count();
    if (count > 0) {
        char label[32];
        snprintf(label, sizeof(label), "\360\237\224\224 %u", count);  /* bell emoji + count */
        gtk_button_set_label(GTK_BUTTON(ui.bell_button), label);
    } else {
        gtk_button_set_label(GTK_BUTTON(ui.bell_button), "\360\237\224\224");
    }
}

static void
navigate_to_notification_target(int ws_idx, GtkNotebook *pane_notebook, int tab_idx)
{
    if (ws_idx >= 0 && workspaces &&
        ws_idx < (int)workspaces->len) {
        workspace_switch(ws_idx, ui.terminal_stack, ui.workspace_list);
    }

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

static gboolean
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

static void
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
    apply_toast_position_setting();
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

static void
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
    apply_toast_position_setting();

    gtk_label_set_text(GTK_LABEL(ui.toast_label), msg);
    gtk_revealer_set_reveal_child(GTK_REVEALER(ui.toast_revealer), TRUE);
    g_sidebar_toast_timeout_id = g_timeout_add(timeout_ms > 0 ? timeout_ms : 8000,
                                               sidebar_toast_timeout_cb, NULL);
}

static void
sidebar_toast_show(const char *msg, int ws_idx, GtkNotebook *pane_notebook, int tab_idx)
{
    sidebar_toast_show_internal(msg, ws_idx, pane_notebook, tab_idx,
                                FALSE, FALSE, FALSE, 8000);
}

static void
sidebar_toast_show_copy(const char *msg, int ws_idx,
                        GtkNotebook *pane_notebook, int tab_idx)
{
    sidebar_toast_show_internal(msg, ws_idx, pane_notebook, tab_idx,
                                TRUE, TRUE, FALSE, 2000);
}

static void
terminal_search_update_count_label(void)
{
    if (g_terminal_search_target) {
        ghostty_terminal_set_search_results(g_terminal_search_target,
                                            g_terminal_search_total,
                                            g_terminal_search_selected);
    }
}

static void
terminal_search_send_action(GhosttyTerminal *term, const char *action)
{
    ghostty_surface_t surface;

    if (!term || !action || !action[0])
        return;

    surface = ghostty_terminal_get_surface(term);
    if (!surface)
        return;

    ghostty_surface_binding_action(surface, action, strlen(action));
}

static void
terminal_search_hide(void)
{
    if (g_terminal_search_target) {
        ghostty_terminal_set_search_active(g_terminal_search_target, FALSE, "");
        ghostty_terminal_focus(g_terminal_search_target);
        g_object_unref(g_terminal_search_target);
        g_terminal_search_target = NULL;
    }

    g_terminal_search_total = -1;
    g_terminal_search_selected = -1;
    terminal_search_update_count_label();
}

static void
terminal_search_set_target(GhosttyTerminal *term)
{
    if (term == g_terminal_search_target)
        return;

    if (g_terminal_search_target)
        g_object_unref(g_terminal_search_target);

    g_terminal_search_target = term ? g_object_ref(term) : NULL;
    g_terminal_search_total = -1;
    g_terminal_search_selected = -1;
    terminal_search_update_count_label();
}

static void
terminal_search_show(GhosttyTerminal *term, const char *needle)
{
    terminal_search_set_target(term);
    if (!term)
        return;

    ghostty_terminal_set_search_active(term, TRUE, needle ? needle : "");
    terminal_search_update_count_label();
}

/* ── Notification click-to-navigate ── */

/*
 * Data passed to notification row click handlers so we can navigate
 * to the correct workspace / pane / tab.
 */
typedef struct {
    int  workspace_idx;
    GtkNotebook *pane_notebook;
    int  tab_idx;
    GtkWidget *popover;      /* so we can close the popover */
} NotifNavData;

static void
on_notif_row_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    NotifNavData *nav = user_data;

    navigate_to_notification_target(nav->workspace_idx,
                                    nav->pane_notebook,
                                    nav->tab_idx);

    /* Close the popover */
    if (nav->popover && GTK_IS_POPOVER(nav->popover))
        gtk_popover_popdown(GTK_POPOVER(nav->popover));
}

/* Bell button clicked: show notification popover with clickable rows */
static void
on_bell_button_clicked(GtkButton *btn, gpointer user_data)
{
    (void)user_data;
    GtkWidget *widget = GTK_WIDGET(btn);

    notifications_init();

    /* Build a custom popover with clickable rows instead of a GMenu */
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, widget);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
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
        guint i;
        /* Show most recent first */
        for (i = g_notifications->len; i > 0; i--) {
            NotificationEntry *e = g_ptr_array_index(g_notifications, i - 1);
            GtkWidget *row_btn = gtk_button_new_with_label(e->text);
            gtk_widget_set_hexpand(row_btn, TRUE);
            gtk_button_set_has_frame(GTK_BUTTON(row_btn), FALSE);

            NotifNavData *nav = g_new0(NotifNavData, 1);
            nav->workspace_idx = e->workspace_idx;
            nav->pane_notebook = e->pane_notebook;
            nav->tab_idx = e->tab_idx;
            nav->popover = popover;
            g_object_set_data_full(G_OBJECT(row_btn), "notif-nav-data",
                                   nav, g_free);

            g_signal_connect(row_btn, "clicked",
                             G_CALLBACK(on_notif_row_clicked), nav);
            gtk_box_append(GTK_BOX(vbox), row_btn);
        }

        /* Separator + Clear All */
        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_margin_top(sep, 4);
        gtk_widget_set_margin_bottom(sep, 4);
        gtk_box_append(GTK_BOX(vbox), sep);

        GtkWidget *clear_btn = gtk_button_new_with_label("Clear All");
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

// ── Ghostty callbacks ──

static gboolean wakeup_idle(gpointer data) {
    (void)data;
    if (g_ghostty_app)
        ghostty_app_tick(g_ghostty_app);
    return G_SOURCE_REMOVE;
}

static void wakeup_cb(void *ud) {
    (void)ud;
    g_idle_add(wakeup_idle, NULL);
}
static void close_surface_cb(void *ud, _Bool rt) { (void)ud; (void)rt; }
static bool read_clipboard_cb(void *ud, ghostty_clipboard_e c, void *d) {
    (void)ud; (void)c; (void)d; return false;
}
static void confirm_read_clipboard_cb(void *ud, const char *t, void *d, ghostty_clipboard_request_e r) {
    (void)ud; (void)t; (void)d; (void)r;
}
static void write_clipboard_cb(void *ud, ghostty_clipboard_e c, const ghostty_clipboard_content_s *co, size_t l, _Bool cf) {
    (void)ud; (void)c; (void)co; (void)l; (void)cf;
}

// ── Browser tab management ──

static void add_browser_tab(const char *url);

static void on_browser_title_changed(BrowserTab *bt, const char *title, gpointer lbl) {
    (void)bt;
    /* Bug 12 fix: guard against NULL title */
    const char *safe_title = title ? title : "";
    char s[24]; snprintf(s, sizeof(s), "%.20s", safe_title);
    gtk_label_set_text(GTK_LABEL(lbl), s);
}

static void on_browser_new_tab_requested(BrowserTab *bt, const char *url, gpointer d) {
    (void)bt; (void)d;
    add_browser_tab(url);
    session_queue_save();
}

static void
about_dialog_hide(GtkWindow *window)
{
    if (!window)
        return;
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
}

static gboolean
on_about_close_request(GtkWindow *window, gpointer user_data)
{
    (void)user_data;
    about_dialog_hide(window);
    return TRUE;
}

static void
about_dialog_present(GtkWindow *parent)
{
    if (!g_about_window) {
        GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
        g_autofree char *logo_path = NULL;
        GtkWidget *logo = NULL;
        GtkWidget *title = gtk_label_new("PrettyMux");
        GtkWidget *version = gtk_label_new(PRETTYMUX_VERSION);
        GtkWidget *subtitle = gtk_label_new("GPU-accelerated terminal multiplexer");
        GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget *license = gtk_label_new(NULL);
        GtkWidget *website = gtk_link_button_new_with_label(PRETTYMUX_WEBSITE_URL,
                                                            "Official website");
        GtkWidget *github = gtk_link_button_new_with_label(PRETTYMUX_GITHUB_URL,
                                                           "GitHub");
        GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *close_btn = gtk_button_new_with_label("Close");

        logo_path = g_build_filename(PRETTYMUX_SOURCE_DIR, "..", "..",
                                     "packaging", "prettymux.svg", NULL);
        if (g_file_test(logo_path, G_FILE_TEST_EXISTS))
            logo = gtk_image_new_from_file(logo_path);
        else
            logo = gtk_image_new_from_icon_name("prettymux");

        g_about_window = GTK_WINDOW(gtk_window_new());
        gtk_window_set_title(g_about_window, "About PrettyMux");
        gtk_window_set_modal(g_about_window, TRUE);
        gtk_window_set_default_size(g_about_window, 440, 360);
        g_signal_connect(g_about_window, "close-request",
                         G_CALLBACK(on_about_close_request), NULL);

        gtk_widget_set_margin_top(outer, 24);
        gtk_widget_set_margin_bottom(outer, 24);
        gtk_widget_set_margin_start(outer, 24);
        gtk_widget_set_margin_end(outer, 24);

        gtk_image_set_pixel_size(GTK_IMAGE(logo), 96);
        gtk_widget_set_halign(logo, GTK_ALIGN_CENTER);

        gtk_label_set_markup(GTK_LABEL(title),
                             "<span size='xx-large' weight='bold'>PrettyMux</span>");
        gtk_widget_set_halign(title, GTK_ALIGN_CENTER);

        gtk_label_set_markup(GTK_LABEL(version),
                             "<span alpha='80%'>Version " PRETTYMUX_VERSION "</span>");
        gtk_widget_set_halign(version, GTK_ALIGN_CENTER);

        gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
        gtk_label_set_justify(GTK_LABEL(subtitle), GTK_JUSTIFY_CENTER);
        gtk_widget_set_halign(subtitle, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(subtitle, "dim-label");

        gtk_label_set_markup(GTK_LABEL(license),
                             "<b>License:</b> " PRETTYMUX_LICENSE_NAME);
        gtk_label_set_xalign(GTK_LABEL(license), 0.0f);

        gtk_widget_set_halign(website, GTK_ALIGN_START);
        gtk_widget_set_halign(github, GTK_ALIGN_START);

        gtk_box_append(GTK_BOX(info), license);
        gtk_box_append(GTK_BOX(info), website);
        gtk_box_append(GTK_BOX(info), github);

        gtk_widget_set_halign(buttons, GTK_ALIGN_END);
        g_signal_connect_swapped(close_btn, "clicked",
                                 G_CALLBACK(about_dialog_hide), g_about_window);
        gtk_box_append(GTK_BOX(buttons), close_btn);

        gtk_box_append(GTK_BOX(outer), logo);
        gtk_box_append(GTK_BOX(outer), title);
        gtk_box_append(GTK_BOX(outer), version);
        gtk_box_append(GTK_BOX(outer), subtitle);
        gtk_box_append(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        gtk_box_append(GTK_BOX(outer), info);
        gtk_box_append(GTK_BOX(outer), buttons);

        gtk_window_set_child(g_about_window, outer);
    }

    if (parent)
        gtk_window_set_transient_for(g_about_window, parent);
    gtk_window_present(g_about_window);
}

static void add_browser_tab(const char *url) {
    GtkWidget *tab = browser_tab_new(url);
    GtkWidget *label = gtk_label_new("Loading...");

    /* Bug 11 fix: tie signal lifetime to the label widget so the handler
     * is automatically disconnected if the label is destroyed first. */
    g_signal_connect_object(tab, "title-changed",
                            G_CALLBACK(on_browser_title_changed), label, 0);
    g_signal_connect(tab, "new-tab-requested", G_CALLBACK(on_browser_new_tab_requested), NULL);

    int idx = gtk_notebook_append_page(GTK_NOTEBOOK(ui.browser_notebook), tab, label);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(ui.browser_notebook), tab, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(ui.browser_notebook), idx);
    gtk_widget_set_visible(tab, TRUE);
}

// ── Clipboard paste callback ──

static void
on_clipboard_text_received(GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
    ghostty_surface_t surface = (ghostty_surface_t)user_data;
    GError *error = NULL;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source),
                                                 result, &error);
    if (text && text[0] && surface) {
        ghostty_surface_text(surface, text, strlen(text));
    }
    g_free(text);
    if (error)
        g_error_free(error);
}

// ── Action dispatch ──

static void save_session_now(void);
static gboolean quit_window_idle_cb(gpointer data);
static void request_close_current_browser_tab(void);
static void request_close_current_tab(Workspace *ws);
static void request_close_current_pane(Workspace *ws);
static void request_close_current_workspace(void);

static void handle_action(const char *action) {
    if (g_str_has_prefix(action, "workspace.focus.")) {
        const char *suffix = action + strlen("workspace.focus.");
        int idx = atoi(suffix) - 1;
        if (workspaces && idx >= 0 && idx < (int)workspaces->len)
            workspace_switch(idx, ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "workspace.new") == 0) {
        workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
    } else if (strcmp(action, "workspace.close") == 0) {
        request_close_current_workspace();
    } else if (strcmp(action, "workspace.next") == 0) {
        if (!workspaces || workspaces->len == 0) return;
        workspace_switch((current_workspace + 1) % workspaces->len,
                         ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "workspace.prev") == 0) {
        if (!workspaces || workspaces->len == 0) return;
        workspace_switch((current_workspace - 1 + workspaces->len) % workspaces->len,
                         ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "pane.tab.new") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) {
            GtkNotebook *focused = workspace_get_focused_pane(ws);
            if (focused && GTK_IS_NOTEBOOK(focused)) {
                /* Declared in workspace.c as static; use the public API
                 * which adds to the first notebook.  For focused-pane
                 * support, we call workspace_add_terminal which now goes
                 * to the focused pane. */
                workspace_add_terminal_to_focused(ws, g_ghostty_app);
            } else {
                workspace_add_terminal(ws, g_ghostty_app);
            }
        }
    } else if (strcmp(action, "pane.focus.left") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_navigate_pane(ws, -1, 0);
    } else if (strcmp(action, "pane.focus.right") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_navigate_pane(ws, 1, 0);
    } else if (strcmp(action, "pane.focus.up") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_navigate_pane(ws, 0, -1);
    } else if (strcmp(action, "pane.focus.down") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_navigate_pane(ws, 0, 1);
    } else if (strcmp(action, "browser.toggle") == 0) {
        gboolean vis = gtk_widget_get_visible(ui.browser_notebook);
        gtk_widget_set_visible(ui.browser_notebook, !vis);
    } else if (strcmp(action, "browser.new") == 0) {
        add_browser_tab("https://prettymux-web.vercel.app/?prettymux=t");
        gtk_widget_set_visible(ui.browser_notebook, TRUE);
    } else if (strcmp(action, "browser.tab.new") == 0) {
        add_browser_tab("https://prettymux-web.vercel.app/?prettymux=t");
        gtk_widget_set_visible(ui.browser_notebook, TRUE);
    } else if (strcmp(action, "browser.tab.close") == 0) {
        request_close_current_browser_tab();
    } else if (strcmp(action, "devtools.docked") == 0 || strcmp(action, "devtools.window") == 0) {
        int pg = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui.browser_notebook));
        if (pg >= 0) {
            GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(ui.browser_notebook), pg);
            if (BROWSER_IS_TAB(child)) browser_tab_show_inspector(BROWSER_TAB(child));
        }
    } else if (strcmp(action, "settings.show") == 0) {
        if (g_main_window)
            settings_dialog_present(g_main_window, apply_runtime_settings, NULL);
    } else if (strcmp(action, "about.show") == 0) {
        if (g_main_window)
            about_dialog_present(g_main_window);
    } else if (strcmp(action, "theme.cycle") == 0) {
        theme_cycle();
        sync_ghostty_theme_to_prettymux_theme();
        app_settings_save();
        apply_runtime_settings(NULL);
    } else if (strcmp(action, "tab.close") == 0) {
        request_close_current_tab(workspace_get_current());
    } else if (strcmp(action, "pane.tab.move") == 0) {
        if (ui.overlay)
            pane_move_overlay_toggle(GTK_OVERLAY(ui.overlay),
                                     ui.terminal_stack,
                                     ui.workspace_list);
    } else if (strcmp(action, "pane.close") == 0) {
        request_close_current_pane(workspace_get_current());
    } else if (strcmp(action, "broadcast.toggle") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) ws->broadcast = !ws->broadcast;
    } else if (strcmp(action, "split.horizontal") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_split_pane(ws, GTK_ORIENTATION_HORIZONTAL, g_ghostty_app);
    } else if (strcmp(action, "split.vertical") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) workspace_split_pane(ws, GTK_ORIENTATION_VERTICAL, g_ghostty_app);
    } else if (strcmp(action, "window.fullscreen") == 0) {
        if (gtk_window_is_fullscreen(g_main_window))
            gtk_window_unfullscreen(g_main_window);
        else
            gtk_window_fullscreen(g_main_window);
    } else if (strcmp(action, "search.show") == 0) {
        if (ui.command_palette)
            command_palette_toggle(COMMAND_PALETTE(ui.command_palette));
    } else if (strcmp(action, "shortcuts.show") == 0) {
        /* Feature 1: Shortcuts overlay */
        if (ui.overlay)
            shortcuts_overlay_toggle(GTK_OVERLAY(ui.overlay));
    } else if (strcmp(action, "pip.toggle") == 0) {
        /* Feature 2: Picture-in-Picture window */
        pip_window_toggle(g_main_window, ui.browser_notebook);
    } else if (strcmp(action, "pane.zoom") == 0) {
        /* Feature 3: Pane zoom */
        Workspace *ws = workspace_get_current();
        if (ws) workspace_toggle_zoom(ws);
    } else if (strcmp(action, "notes.toggle") == 0) {
        /* Feature 4: Notes panel */
        Workspace *ws = workspace_get_current();
        if (ws && ui.terminal_box)
            workspace_toggle_notes(ws, ui.terminal_box);
    } else if (strcmp(action, "terminal.search") == 0) {
        /* Feature 5: Terminal search via ghostty built-in */
        Workspace *ws = workspace_get_current();
        if (ws) {
            GtkNotebook *focused = workspace_get_focused_pane(ws);
            if (focused && GTK_IS_NOTEBOOK(focused)) {
                int pg = gtk_notebook_get_current_page(focused);
                if (pg >= 0) {
                    GhosttyTerminal *term = notebook_terminal_at(focused, pg);
                    if (term) {
                        ghostty_surface_t surface =
                            ghostty_terminal_get_surface(term);
                        if (surface) {
                            ghostty_surface_binding_action(surface,
                                                           "start_search", 12);
                            terminal_search_show(term, "");
                        }
                    }
                }
            }
        }
    } else if (strcmp(action, "browser.focus_url") == 0) {
        /* Ctrl+L: focus the URL bar in the current browser tab */
        if (gtk_widget_get_visible(ui.browser_notebook)) {
            int pg = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui.browser_notebook));
            if (pg >= 0) {
                GtkWidget *child = gtk_notebook_get_nth_page(
                    GTK_NOTEBOOK(ui.browser_notebook), pg);
                if (BROWSER_IS_TAB(child))
                    browser_tab_focus_url(BROWSER_TAB(child));
            }
        }
    } else if (strcmp(action, "terminal.copy") == 0) {
        /* Ctrl+Shift+C: copy terminal selection to clipboard */
        Workspace *ws = workspace_get_current();
        if (ws) {
            GtkNotebook *focused = workspace_get_focused_pane(ws);
            if (focused && GTK_IS_NOTEBOOK(focused)) {
                int pg = gtk_notebook_get_current_page(focused);
                if (pg >= 0) {
                    GhosttyTerminal *term = notebook_terminal_at(focused, pg);
                    if (term) {
                        ghostty_surface_t surface =
                            ghostty_terminal_get_surface(term);
                        if (surface && ghostty_surface_has_selection(surface)) {
                            ghostty_text_s text = {0};
                            if (ghostty_surface_read_selection(surface, &text)) {
                                if (text.text && text.text_len > 0) {
                                    GdkClipboard *clip = gdk_display_get_clipboard(
                                        gdk_display_get_default());
                                    gdk_clipboard_set_text(clip, text.text);
                                    sidebar_toast_show_copy("Copied to clipboard",
                                                            current_workspace,
                                                            focused, pg);
                                }
                                ghostty_surface_free_text(surface, &text);
                            }
                        }
                    }
                }
            }
        }
    } else if (strcmp(action, "terminal.paste") == 0) {
        /* Ctrl+Shift+V: paste from clipboard into terminal */
        Workspace *ws = workspace_get_current();
        if (ws) {
            GtkNotebook *focused = workspace_get_focused_pane(ws);
            if (focused && GTK_IS_NOTEBOOK(focused)) {
                int pg = gtk_notebook_get_current_page(focused);
                if (pg >= 0) {
                    GhosttyTerminal *term = notebook_terminal_at(focused, pg);
                    if (term) {
                        ghostty_surface_t surface =
                            ghostty_terminal_get_surface(term);
                        if (surface) {
                            GdkClipboard *clip = gdk_display_get_clipboard(
                                gdk_display_get_default());
                            /* Read clipboard text asynchronously.
                             * Store the surface pointer for the callback. */
                            gdk_clipboard_read_text_async(clip, NULL,
                                on_clipboard_text_received, surface);
                        }
                    }
                }
            }
        }
    } else if (strcmp(action, "recording.mark") == 0) {
        Workspace *ws = workspace_get_current();
        GtkNotebook *pane = ws ? workspace_get_focused_pane(ws) : NULL;
        int tab_idx = (pane && GTK_IS_NOTEBOOK(pane))
            ? gtk_notebook_get_current_page(pane) : -1;
        sidebar_toast_show_internal("Recording marker",
                                    current_workspace,
                                    pane, tab_idx,
                                    FALSE, FALSE, TRUE, 8000);
    }

    session_queue_save();
}

// ── Keyboard handler (capture phase) ──

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state, gpointer d)
{
    (void)ctrl; (void)keycode; (void)d;
    GdkModifierType mods = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                     GDK_ALT_MASK | GDK_SUPER_MASK);
    guint lower = gdk_keyval_to_lower(keyval);

    if (g_terminal_search_target && focus_within_terminal(g_terminal_search_target) &&
        terminal_search_handle_key(keyval, state))
        return TRUE;

    /* ── Ctrl+Tab / Ctrl+Shift+Tab: cycle terminal tabs ── */
    if ((lower == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) &&
        (mods == GDK_CONTROL_MASK ||
         mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
        Workspace *ws = workspace_get_current();
        if (ws) {
            GtkNotebook *nb = workspace_get_focused_pane(ws);
            if (nb) {
                int n = gtk_notebook_get_n_pages(nb);
                if (n > 1) {
                    int cur = gtk_notebook_get_current_page(nb);
                    int next;
                    if (mods & GDK_SHIFT_MASK)
                        next = (cur - 1 + n) % n;
                    else
                        next = (cur + 1) % n;
                    gtk_notebook_set_current_page(nb, next);
                }
            }
        }
        return TRUE;
    }

    /* ── Shortcut table lookup ── */
    const char *action = shortcut_match(keyval, state);
    if (action) {
        const ShortcutDef *binding = shortcut_find_by_action(action);
        g_autofree char *keys = shortcut_format_binding(binding);

        /* browser.tab.close (Ctrl+W) should only fire when the browser
           actually has keyboard focus; otherwise let ghostty handle it. */
        if (strcmp(action, "browser.tab.close") == 0) {
            GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(g_main_window));
            if (!focus || !gtk_widget_is_ancestor(focus, ui.browser_notebook))
                return FALSE;
        }
        if (strcmp(action, "recording.mark") != 0)
            shortcut_log_event("shortcut", action, keys);
        handle_action(action);
        return TRUE;
    }

    Workspace *ws = workspace_get_current();
    if (ws && ws->broadcast && !is_modifier_key(keyval)) {
        GtkNotebook *focused = workspace_get_focused_pane(ws);
        if (focused) {
            int pg = gtk_notebook_get_current_page(focused);
            GhosttyTerminal *source = notebook_terminal_at(focused, pg);
            if (source && focus_within_terminal(source))
                broadcast_key_to_workspace(ws, source, keyval, keycode, state);
        }
    }
    return FALSE;
}

// ── Terminal lookup: find GhosttyTerminal by ghostty_surface_t ──

typedef struct {
    GhosttyTerminal *terminal;
    Workspace       *workspace;
    int              workspace_idx;
    GtkNotebook     *pane_notebook;
    int              pane_idx;
    int              tab_idx;
} SurfaceLookup;

static SurfaceLookup
find_terminal_for_surface(ghostty_surface_t surface)
{
    SurfaceLookup result = { NULL, NULL, -1, NULL, -1, -1 };
    if (!surface || !workspaces)
        return result;

    for (guint wi = 0; wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);
        for (guint ti = 0; ti < ws->terminals->len; ti++) {
            GhosttyTerminal *term = g_ptr_array_index(ws->terminals, ti);
            if (ghostty_terminal_get_surface(term) == surface) {
                result.terminal = term;
                result.workspace = ws;
                result.workspace_idx = (int)wi;

                /* Find which pane notebook and tab index this terminal is in */
                if (ws->pane_notebooks) {
                    guint pi;
                    for (pi = 0; pi < ws->pane_notebooks->len; pi++) {
                        GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
                        int n_pages = gtk_notebook_get_n_pages(nb);
                        int pg;
                        for (pg = 0; pg < n_pages; pg++) {
                            if (GTK_WIDGET(notebook_terminal_at(nb, pg)) ==
                                GTK_WIDGET(term)) {
                                result.pane_notebook = nb;
                                result.pane_idx = (int)pi;
                                result.tab_idx = pg;
                                return result;
                            }
                        }
                    }
                }
                return result;
            }
        }
    }
    return result;
}

static SurfaceLookup
find_terminal_for_id(const char *terminal_id)
{
    SurfaceLookup result = { NULL, NULL, -1, NULL, -1, -1 };

    if (!terminal_id || !terminal_id[0] || !workspaces)
        return result;

    for (guint wi = 0; wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);
        if (!ws || !ws->terminals)
            continue;

        for (guint ti = 0; ti < ws->terminals->len; ti++) {
            GhosttyTerminal *term = g_ptr_array_index(ws->terminals, ti);
            if (!term)
                continue;
            if (g_strcmp0(ghostty_terminal_get_id(term), terminal_id) != 0)
                continue;

            result.terminal = term;
            result.workspace = ws;
            result.workspace_idx = (int)wi;

            if (!ws->pane_notebooks)
                return result;

            for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
                GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
                int n_pages = gtk_notebook_get_n_pages(nb);
                for (int pg = 0; pg < n_pages; pg++) {
                    if (notebook_terminal_at(nb, pg) == term) {
                        result.pane_notebook = nb;
                        result.pane_idx = (int)pi;
                        result.tab_idx = pg;
                        return result;
                    }
                }
            }

            return result;
        }
    }

    return result;
}

static gboolean
workspace_should_show_port_notification(Workspace *ws)
{
    if (!ws)
        return FALSE;

    return !(ws->name[0] == '\0' ||
             strcmp(ws->name, "bash") == 0 ||
             strcmp(ws->name, "zsh") == 0 ||
             strcmp(ws->name, "fish") == 0 ||
             strcmp(ws->name, "sh") == 0);
}

static void
on_port_scanner_detected(const char *terminal_id, int port, gpointer user_data)
{
    (void)user_data;
    handle_reported_port(terminal_id, port);
}

static void
register_terminal_scope(const char *terminal_id,
                        pid_t       session_id,
                        const char *tty_name,
                        const char *tty_path)
{
    SurfaceLookup loc;

    if (!terminal_id || !terminal_id[0])
        return;

    loc = find_terminal_for_id(terminal_id);
    if (!loc.terminal)
        return;

    ghostty_terminal_set_scope(loc.terminal, session_id, tty_name, tty_path);
    port_scanner_register_terminal(terminal_id, session_id, tty_name,
                                   tty_path, loc.workspace_idx);
}

static void
handle_reported_port(const char *terminal_id, int port)
{
    if (!terminal_id || !terminal_id[0] || port <= 0)
        return;

    SurfaceLookup loc = find_terminal_for_id(terminal_id);
    Workspace *ws = loc.workspace;
    char msg[64];
    char notif_msg[96];

    if (loc.workspace_idx >= 0)
        port_scanner_set_terminal_workspace(terminal_id, loc.workspace_idx);

    if (!workspace_should_show_port_notification(ws))
        return;

    snprintf(ws->notification, sizeof(ws->notification),
             "-> localhost:%d", port);
    workspace_refresh_sidebar_label(ws);

    snprintf(msg, sizeof(msg), "Port %d detected", port);
    snprintf(notif_msg, sizeof(notif_msg), "Port %d in %s",
             port, ws->name[0] ? ws->name : "workspace");
    notifications_add_full(notif_msg, loc.workspace_idx,
                           loc.pane_notebook, loc.tab_idx);
    bell_button_update();
    workspace_mark_tab_notification(loc.pane_notebook, loc.tab_idx);
    if (!notification_target_is_active(loc.workspace_idx,
                                       loc.pane_notebook,
                                       loc.tab_idx)) {
        if (g_main_window_active) {
            debug_notification_log(
                "notify route port toast active=%d target=(%d,%d,%d) msg=%s",
                g_main_window_active,
                loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                notif_msg);
            sidebar_toast_show(notif_msg, loc.workspace_idx,
                               loc.pane_notebook, loc.tab_idx);
        } else {
            debug_notification_log(
                "notify route port desktop active=%d target=(%d,%d,%d) msg=%s",
                g_main_window_active,
                loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                msg);
            send_desktop_notification("PrettyMux", msg,
                                      loc.workspace_idx,
                                      loc.pane_idx,
                                      loc.tab_idx);
        }
    } else {
        debug_notification_log(
            "notify route port suppressed active-target target=(%d,%d,%d) msg=%s",
            loc.workspace_idx, loc.pane_idx, loc.tab_idx,
            notif_msg);
    }
}

// ── Socket server callback ──

static void
on_socket_command(const char  *command,
                  JsonObject  *msg,
                  JsonBuilder *response,
                  gpointer     user_data)
{
    (void)user_data;

    if (strcmp(command, "tabs.list") == 0) {
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "activeWorkspace");
        json_builder_add_int_value(response, current_workspace);
        json_builder_set_member_name(response, "workspaces");
        json_builder_begin_array(response);
        if (workspaces) {
            guint wi;
            for (wi = 0; wi < workspaces->len; wi++) {
                Workspace *ws = g_ptr_array_index(workspaces, wi);
                json_builder_begin_object(response);
                json_builder_set_member_name(response, "index");
                json_builder_add_int_value(response, (int)wi);
                json_builder_set_member_name(response, "name");
                json_builder_add_string_value(response, ws->name);
                json_builder_set_member_name(response, "active");
                json_builder_add_boolean_value(response,
                    (int)wi == current_workspace);
                json_builder_set_member_name(response, "panes");
                json_builder_begin_array(response);
                if (ws->pane_notebooks) {
                    GtkNotebook *focused_pane = workspace_get_focused_pane(ws);
                    guint pi;
                    for (pi = 0; pi < ws->pane_notebooks->len; pi++) {
                        GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
                        json_builder_begin_object(response);
                        json_builder_set_member_name(response, "id");
                        json_builder_add_string_value(response,
                            workspace_get_pane_id(nb));
                        json_builder_set_member_name(response, "index");
                        json_builder_add_int_value(response, (int)pi);
                        json_builder_set_member_name(response, "focused");
                        json_builder_add_boolean_value(response, nb == focused_pane);
                        json_builder_set_member_name(response, "activeTab");
                        json_builder_add_int_value(response,
                            GTK_IS_NOTEBOOK(nb)
                                ? gtk_notebook_get_current_page(nb)
                                : -1);
                        json_builder_set_member_name(response, "tabs");
                        json_builder_begin_array(response);
                        if (GTK_IS_NOTEBOOK(nb)) {
                            int n_pages = gtk_notebook_get_n_pages(nb);
                            int ti;
                            for (ti = 0; ti < n_pages; ti++) {
                                GtkWidget *child =
                                    gtk_notebook_get_nth_page(nb, ti);
                                const char *tab_name = "Terminal";
                                gboolean is_custom = FALSE;
                                GtkWidget *tab_widget =
                                    gtk_notebook_get_tab_label(nb, child);
                                if (tab_widget) {
                                    for (GtkWidget *w = gtk_widget_get_first_child(tab_widget);
                                         w; w = gtk_widget_get_next_sibling(w)) {
                                        if (GTK_IS_LABEL(w)) {
                                            tab_name = gtk_label_get_text(GTK_LABEL(w));
                                            if (g_object_get_data(G_OBJECT(w), "user-renamed"))
                                                is_custom = TRUE;
                                            break;
                                        }
                                    }
                                }

                                const char *pwd = NULL;
                                GtkWidget *terminal = page_linked_terminal(child);
                                if (terminal)
                                    pwd = ghostty_terminal_get_cwd(
                                        GHOSTTY_TERMINAL(terminal));

                                json_builder_begin_object(response);
                                json_builder_set_member_name(response, "index");
                                json_builder_add_int_value(response, ti);
                                json_builder_set_member_name(response, "name");
                                json_builder_add_string_value(response,
                                    tab_name ? tab_name : "Terminal");
                                json_builder_set_member_name(response, "customName");
                                json_builder_add_boolean_value(response, is_custom);
                                json_builder_set_member_name(response, "pwd");
                                json_builder_add_string_value(response, pwd ? pwd : "");
                                json_builder_set_member_name(response, "active");
                                json_builder_add_boolean_value(response,
                                    ti == gtk_notebook_get_current_page(nb));
                                json_builder_end_object(response);
                            }
                        }
                        json_builder_end_array(response);
                        json_builder_end_object(response);
                    }
                }
                json_builder_end_array(response);
                json_builder_end_object(response);
            }
        }
        json_builder_end_array(response);
    } else if (strcmp(command, "browser.open") == 0) {
        const char *url = json_object_get_string_member_with_default(
            msg, "url", "");
        if (url && url[0]) {
            add_browser_tab(url);
            gtk_widget_set_visible(ui.browser_notebook, TRUE);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing url");
        }
    } else if (strcmp(command, "workspace.new") == 0) {
        const char *name = json_object_get_string_member_with_default(
            msg, "name", "");
        workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
        if (name && name[0] && workspaces && workspaces->len > 0) {
            Workspace *ws = g_ptr_array_index(workspaces,
                                               workspaces->len - 1);
            snprintf(ws->name, sizeof(ws->name), "%.60s", name);
            workspace_refresh_sidebar_label(ws);
        }
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "index");
        json_builder_add_int_value(response,
            workspaces ? (int)workspaces->len - 1 : 0);
    } else if (strcmp(command, "workspace.list") == 0) {
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "workspaces");
        json_builder_begin_array(response);
        if (workspaces) {
            guint i;
            for (i = 0; i < workspaces->len; i++) {
                Workspace *ws = g_ptr_array_index(workspaces, i);
                json_builder_begin_object(response);
                json_builder_set_member_name(response, "index");
                json_builder_add_int_value(response, (int)i);
                json_builder_set_member_name(response, "name");
                json_builder_add_string_value(response, ws->name);
                json_builder_set_member_name(response, "active");
                json_builder_add_boolean_value(response,
                    (int)i == current_workspace);
                json_builder_end_object(response);
            }
        }
        json_builder_end_array(response);
    } else if (strcmp(command, "workspace.current") == 0) {
        Workspace *ws = workspace_get_current();
        GtkNotebook *pane = ws ? workspace_get_focused_pane(ws) : NULL;
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "index");
        json_builder_add_int_value(response, current_workspace);
        json_builder_set_member_name(response, "name");
        json_builder_add_string_value(response, ws ? ws->name : "");
        json_builder_set_member_name(response, "paneId");
        json_builder_add_string_value(response,
            pane ? workspace_get_pane_id(pane) : "");
        json_builder_set_member_name(response, "paneIndex");
        json_builder_add_int_value(response,
            (ws && pane) ? workspace_get_pane_index(ws, pane) : -1);
    } else if (strcmp(command, "workspace.switch") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "index", -1);
        if (idx >= 0 && workspaces && idx < (int)workspaces->len) {
            workspace_switch(idx, ui.terminal_stack, ui.workspace_list);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace index");
        }
    } else if (strcmp(command, "workspace.equalize_splits") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        const char *orientation =
            json_object_get_string_member_with_default(msg, "orientation", "");
        Workspace *ws = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);

        if (ws && workspace_equalize_splits(ws,
                                            (orientation && orientation[0])
                                                ? orientation
                                                : NULL)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "equalize failed");
        }
    } else if (strcmp(command, "pane.list") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        Workspace *ws = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);

        if (!ws) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "invalid workspace");
        } else {
            GtkNotebook *focused_pane = workspace_get_focused_pane(ws);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "workspace");
            json_builder_add_int_value(response, idx);
            json_builder_set_member_name(response, "activePane");
            json_builder_add_int_value(response,
                focused_pane ? workspace_get_pane_index(ws, focused_pane) : -1);
            json_builder_set_member_name(response, "activePaneId");
            json_builder_add_string_value(response,
                focused_pane ? workspace_get_pane_id(focused_pane) : "");
            json_builder_set_member_name(response, "panes");
            json_builder_begin_array(response);
            if (ws->pane_notebooks) {
                for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
                    GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
                    json_builder_begin_object(response);
                    json_builder_set_member_name(response, "id");
                    json_builder_add_string_value(response,
                        workspace_get_pane_id(nb));
                    json_builder_set_member_name(response, "index");
                    json_builder_add_int_value(response, (int)pi);
                    json_builder_set_member_name(response, "focused");
                    json_builder_add_boolean_value(response, nb == focused_pane);
                    json_builder_set_member_name(response, "activeTab");
                    json_builder_add_int_value(response,
                        gtk_notebook_get_current_page(nb));
                    json_builder_set_member_name(response, "tabCount");
                    json_builder_add_int_value(response,
                        gtk_notebook_get_n_pages(nb));
                    json_builder_end_object(response);
                }
            }
            json_builder_end_array(response);
        }
    } else if (strcmp(command, "pane.focus") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        int pane_idx = (int)json_object_get_int_member_with_default(
            msg, "pane", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        Workspace *ws = NULL;
        GtkNotebook *pane = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        if (ws && pane_id && pane_id[0])
            pane = workspace_get_pane_by_id(ws, pane_id);
        if (ws && !pane && pane_idx >= 0)
            pane = workspace_get_pane_by_index(ws, pane_idx);

        if (ws && pane && workspace_focus_pane(ws, pane)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "focus failed");
        }
    } else if (strcmp(command, "pane.split") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        int pane_idx = (int)json_object_get_int_member_with_default(
            msg, "pane", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        const char *direction =
            json_object_get_string_member_with_default(msg, "direction", "right");
        Workspace *ws = NULL;
        GtkNotebook *pane = NULL;
        GtkNotebook *new_pane = NULL;
        GtkOrientation orientation = GTK_ORIENTATION_HORIZONTAL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        if (ws && pane_id && pane_id[0])
            pane = workspace_get_pane_by_id(ws, pane_id);
        if (ws && !pane && pane_idx >= 0)
            pane = workspace_get_pane_by_index(ws, pane_idx);

        if (g_strcmp0(direction, "down") == 0 || g_strcmp0(direction, "up") == 0)
            orientation = GTK_ORIENTATION_VERTICAL;

        if (ws && pane) {
            new_pane = workspace_split_pane_target(ws, pane, orientation,
                                                   g_ghostty_app);
        }

        if (ws && new_pane) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
            json_builder_set_member_name(response, "paneId");
            json_builder_add_string_value(response,
                workspace_get_pane_id(new_pane));
            json_builder_set_member_name(response, "pane");
            json_builder_add_int_value(response,
                workspace_get_pane_index(ws, new_pane));
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "split failed");
        }
    } else if (strcmp(command, "pane.close") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        int pane_idx = (int)json_object_get_int_member_with_default(
            msg, "pane", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        Workspace *ws = NULL;
        GtkNotebook *pane = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        if (ws && pane_id && pane_id[0])
            pane = workspace_get_pane_by_id(ws, pane_id);
        if (ws && !pane && pane_idx >= 0)
            pane = workspace_get_pane_by_index(ws, pane_idx);

        if (ws && pane && ws->pane_notebooks &&
            ws->pane_notebooks->len > 1) {
            workspace_close_pane(ws, pane);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "close failed");
        }
    } else if (strcmp(command, "pane.resize_percent") == 0) {
        int idx = (int)json_object_get_int_member_with_default(
            msg, "workspace", current_workspace);
        int pane_idx = (int)json_object_get_int_member_with_default(
            msg, "pane", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        const char *axis =
            json_object_get_string_member_with_default(msg, "axis", "x");
        double percent = json_object_get_double_member_with_default(
            msg, "percent", -1.0);
        Workspace *ws = NULL;
        GtkNotebook *pane = NULL;

        if (idx >= 0 && workspaces && idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, idx);
        if (ws && pane_id && pane_id[0])
            pane = workspace_get_pane_by_id(ws, pane_id);
        if (ws && !pane && pane_idx >= 0)
            pane = workspace_get_pane_by_index(ws, pane_idx);

        if (ws && pane &&
            workspace_resize_pane_percent(ws, pane, axis[0], percent)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "resize failed");
        }
    } else if (strcmp(command, "tab.new") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) {
            workspace_add_terminal_to_focused(ws, g_ghostty_app);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
        }
    } else if (strcmp(command, "tab.move") == 0) {
        int from_ws = (int)json_object_get_int_member_with_default(msg, "fromWorkspace", -1);
        int from_pane = (int)json_object_get_int_member_with_default(msg, "fromPane", -1);
        int from_tab = (int)json_object_get_int_member_with_default(msg, "fromTab", -1);
        int to_ws = (int)json_object_get_int_member_with_default(msg, "toWorkspace", -1);
        int to_pane = (int)json_object_get_int_member_with_default(msg, "toPane", -1);
        if (workspace_move_tab(from_ws, from_pane, from_tab, to_ws, to_pane)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "move failed");
        }
    } else if (strcmp(command, "tab.select") == 0) {
        int ws_idx = (int)json_object_get_int_member_with_default(msg, "workspace", -1);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        int tab_idx = (int)json_object_get_int_member_with_default(msg, "tab", -1);
        if (workspace_select_tab(ws_idx, pane_idx, tab_idx)) {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "select failed");
        }
    } else if (strcmp(command, "tab.rename") == 0) {
        /* Rename current tab: {"command":"tab.rename","name":"my-tab"} */
        const char *name = json_object_get_string_member_with_default(msg, "name", "");
        if (name && name[0]) {
            Workspace *ws = workspace_get_current();
            if (ws) {
                GtkNotebook *nb = workspace_get_focused_pane(ws);
                if (nb && GTK_IS_NOTEBOOK(nb)) {
                    int pg = gtk_notebook_get_current_page(nb);
                    if (pg >= 0) {
                        GtkWidget *child = gtk_notebook_get_nth_page(nb, pg);
                        GtkWidget *tab_w = gtk_notebook_get_tab_label(nb, child);
                        if (tab_w) {
                            for (GtkWidget *w = gtk_widget_get_first_child(tab_w);
                                 w; w = gtk_widget_get_next_sibling(w)) {
                                if (GTK_IS_LABEL(w)) {
                                    gtk_label_set_text(GTK_LABEL(w), name);
                                    g_object_set_data(G_OBJECT(w), "user-renamed",
                                                      GINT_TO_POINTER(1));
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing name");
        }
    } else if (strcmp(command, "tab.edit") == 0) {
        /* Start inline rename on the current tab */
        Workspace *ws = workspace_get_current();
        if (ws) {
            workspace_start_tab_rename(ws);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
        }
    } else if (strcmp(command, "action") == 0) {
        /* Run any prettymux action by name */
        const char *act = json_object_get_string_member_with_default(msg, "action", "");
        if (act && act[0]) {
            handle_action(act);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing action name");
        }
    } else if (strcmp(command, "type") == 0) {
        /* Type text into the focused terminal: {"command":"type","text":"ls -la\n"} */
        const char *text = json_object_get_string_member_with_default(msg, "text", "");
        /* Optional workspace/pane/tab targeting */
        int ws_idx = (int)json_object_get_int_member_with_default(msg, "workspace", -1);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        int tab_idx = (int)json_object_get_int_member_with_default(msg, "tab", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");

        Workspace *ws = NULL;
        if (ws_idx >= 0 && workspaces && ws_idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, ws_idx);
        else
            ws = workspace_get_current();

        if (ws && text && text[0]) {
            GtkNotebook *nb = NULL;
            if (pane_id && pane_id[0])
                nb = workspace_get_pane_by_id(ws, pane_id);
            if (!nb &&
                pane_idx >= 0 && ws->pane_notebooks && pane_idx < (int)ws->pane_notebooks->len)
                nb = g_ptr_array_index(ws->pane_notebooks, pane_idx);
            else
                nb = nb ? nb : workspace_get_focused_pane(ws);

            GhosttyTerminal *term = NULL;
            if (nb) {
                int pg = (tab_idx >= 0) ? tab_idx : gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
                if (pg >= 0 && pg < gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)))
                    term = notebook_terminal_at(GTK_NOTEBOOK(nb), pg);
            }
            if (term) {
                ghostty_surface_t surface = ghostty_terminal_get_surface(term);
                if (surface) {
                    ghostty_surface_text(surface, text, strlen(text));
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "ok");
                } else {
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "error");
                    json_builder_set_member_name(response, "message");
                    json_builder_add_string_value(response, "terminal has no surface");
                }
            } else {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "error");
                json_builder_set_member_name(response, "message");
                json_builder_add_string_value(response, "no terminal found");
            }
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "missing text");
        }
    } else if (strcmp(command, "exec") == 0) {
        /* Execute a command in a terminal: {"command":"exec","cmd":"ls -la"}
         * This types the command followed by Enter */
        const char *cmd = json_object_get_string_member_with_default(msg, "cmd", "");
        int ws_idx = (int)json_object_get_int_member_with_default(msg, "workspace", -1);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        int tab_idx = (int)json_object_get_int_member_with_default(msg, "tab", -1);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");

        Workspace *ws = NULL;
        if (ws_idx >= 0 && workspaces && ws_idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, ws_idx);
        else
            ws = workspace_get_current();

        if (ws && cmd && cmd[0]) {
            GtkNotebook *nb = NULL;
            if (pane_id && pane_id[0])
                nb = workspace_get_pane_by_id(ws, pane_id);
            if (!nb &&
                pane_idx >= 0 && ws->pane_notebooks && pane_idx < (int)ws->pane_notebooks->len)
                nb = g_ptr_array_index(ws->pane_notebooks, pane_idx);
            else
                nb = nb ? nb : workspace_get_focused_pane(ws);

            GhosttyTerminal *term = NULL;
            if (nb) {
                int pg = (tab_idx >= 0) ? tab_idx : gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
                if (pg >= 0 && pg < gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)))
                    term = notebook_terminal_at(GTK_NOTEBOOK(nb), pg);
            }
            if (term) {
                ghostty_surface_t surface = ghostty_terminal_get_surface(term);
                if (surface) {
                    /* Type command + newline */
                    ghostty_surface_text(surface, cmd, strlen(cmd));
                    ghostty_surface_text(surface, "\n", 1);
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "ok");
                } else {
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "error");
                }
            } else {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "error");
                json_builder_set_member_name(response, "message");
                json_builder_add_string_value(response, "no terminal found");
            }
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
        }
    } else if (strcmp(command, "pane.read_text") == 0) {
        int ws_idx = (int)json_object_get_int_member_with_default(msg, "workspace", -1);
        int pane_idx = (int)json_object_get_int_member_with_default(msg, "pane", -1);
        int tab_idx = (int)json_object_get_int_member_with_default(msg, "tab", -1);
        int lines = (int)json_object_get_int_member_with_default(msg, "lines", 0);
        const char *pane_id =
            json_object_get_string_member_with_default(msg, "paneId", "");
        gboolean scrollback =
            json_object_get_boolean_member_with_default(msg, "scrollback", TRUE);
        Workspace *ws = NULL;
        GtkNotebook *nb = NULL;
        GhosttyTerminal *term = NULL;

        if (ws_idx >= 0 && workspaces && ws_idx < (int)workspaces->len)
            ws = g_ptr_array_index(workspaces, ws_idx);
        else
            ws = workspace_get_current();

        if (ws && pane_id && pane_id[0])
            nb = workspace_get_pane_by_id(ws, pane_id);
        if (!nb && ws &&
            pane_idx >= 0 && ws->pane_notebooks && pane_idx < (int)ws->pane_notebooks->len)
            nb = g_ptr_array_index(ws->pane_notebooks, pane_idx);
        if (!nb && ws)
            nb = workspace_get_focused_pane(ws);

        if (nb) {
            int pg = (tab_idx >= 0) ? tab_idx : gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
            if (pg >= 0 && pg < gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)))
                term = notebook_terminal_at(GTK_NOTEBOOK(nb), pg);
        }

        if (term) {
            ghostty_surface_t surface = ghostty_terminal_get_surface(term);
            if (surface) {
                ghostty_text_s captured = {0};
                ghostty_selection_s sel = {
                    .top_left = {
                        .tag = scrollback ? GHOSTTY_POINT_SCREEN : GHOSTTY_POINT_VIEWPORT,
                        .coord = GHOSTTY_POINT_COORD_TOP_LEFT,
                        .x = 0,
                        .y = 0,
                    },
                    .bottom_right = {
                        .tag = scrollback ? GHOSTTY_POINT_SCREEN : GHOSTTY_POINT_VIEWPORT,
                        .coord = GHOSTTY_POINT_COORD_BOTTOM_RIGHT,
                        .x = 0,
                        .y = 0,
                    },
                    .rectangle = FALSE,
                };

                if (ghostty_surface_read_text(surface, sel, &captured) &&
                    captured.text) {
                    g_autofree char *trimmed =
                        prettymux_trim_last_lines(captured.text, lines);
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "ok");
                    json_builder_set_member_name(response, "text");
                    json_builder_add_string_value(response, trimmed ? trimmed : "");
                    ghostty_surface_free_text(surface, &captured);
                } else {
                    json_builder_set_member_name(response, "status");
                    json_builder_add_string_value(response, "error");
                    json_builder_set_member_name(response, "message");
                    json_builder_add_string_value(response, "read_text failed");
                }
            } else {
                json_builder_set_member_name(response, "status");
                json_builder_add_string_value(response, "error");
                json_builder_set_member_name(response, "message");
                json_builder_add_string_value(response, "terminal has no surface");
            }
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response, "no terminal found");
        }
    } else if (strcmp(command, "port.report") == 0) {
        int port = (int)json_object_get_int_member_with_default(msg, "port", 0);
        const char *terminal_id =
            json_object_get_string_member_with_default(msg, "terminalId", "");

        if (port > 0 && terminal_id[0]) {
            handle_reported_port(terminal_id, port);
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "ok");
        } else {
            json_builder_set_member_name(response, "status");
            json_builder_add_string_value(response, "error");
            json_builder_set_member_name(response, "message");
            json_builder_add_string_value(response,
                                          "missing port or terminalId");
        }
    } else if (strcmp(command, "terminal.register") == 0) {
        const char *terminal_id =
            json_object_get_string_member_with_default(msg, "terminalId", "");
        pid_t session_id = (pid_t)json_object_get_int_member_with_default(msg,
                                                                          "sessionId",
                                                                          0);
        const char *tty_name =
            json_object_get_string_member_with_default(msg, "ttyName", "");
        const char *tty_path =
            json_object_get_string_member_with_default(msg, "ttyPath", "");

        register_terminal_scope(terminal_id, session_id, tty_name, tty_path);
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
    } else if (strcmp(command, "dismiss.welcome") == 0) {
        /* Close any visible dialog by activating the main window */
        if (g_main_window)
            gtk_window_present(g_main_window);
        /* Also write the flag file so it won't show again */
        char *dir = g_build_filename(g_get_home_dir(), ".config", "prettymux", NULL);
        g_mkdir_with_parents(dir, 0755);
        char *flag = g_build_filename(dir, ".welcome-shown", NULL);
        g_file_set_contents(flag, "1", 1, NULL);
        g_free(flag);
        g_free(dir);
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
    } else if (strcmp(command, "app.quit") == 0) {
        save_session_now();
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        g_idle_add(quit_window_idle_cb, NULL);
    } else if (strcmp(command, "list.actions") == 0) {
        /* List all available actions */
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "ok");
        json_builder_set_member_name(response, "actions");
        json_builder_begin_array(response);
        for (int i = 0; default_shortcuts[i].action != NULL; i++) {
            json_builder_add_string_value(response, default_shortcuts[i].action);
        }
        /* Also add actions not in shortcut table */
        json_builder_add_string_value(response, "browser.open");
        json_builder_add_string_value(response, "workspace.new");
        json_builder_add_string_value(response, "workspace.list");
        json_builder_add_string_value(response, "workspace.switch");
        json_builder_add_string_value(response, "tabs.list");
        json_builder_add_string_value(response, "tab.new");
        json_builder_add_string_value(response, "tab.move");
        json_builder_add_string_value(response, "tab.select");
        json_builder_add_string_value(response, "app.quit");
        json_builder_add_string_value(response, "exec");
        json_builder_add_string_value(response, "type");
        json_builder_end_array(response);
    } else {
        json_builder_set_member_name(response, "status");
        json_builder_add_string_value(response, "error");
        json_builder_set_member_name(response, "message");
        json_builder_add_string_value(response, "unknown command");
    }

    /* Defer session save to idle — don't block the socket handler */
    if (strcmp(command, "workspace.list") != 0 &&
        strcmp(command, "tabs.list") != 0 &&
        strcmp(command, "list.actions") != 0 &&
        strcmp(command, "app.quit") != 0 &&
        strcmp(command, "tab.edit") != 0 &&
        strcmp(command, "port.report") != 0) {
        session_queue_save();
    }
}

// ── Ghostty action callback (marshaled to GTK main thread) ──

/*
 * Bug 1 fix: action_cb is called from ghostty's renderer thread.
 * GTK is not thread-safe, so we must marshal the entire action to the
 * GTK main thread via g_idle_add.  We deep-copy any strings that the
 * action references because they are only valid for the duration of
 * the original callback.
 */

typedef struct {
    ghostty_surface_t surface;
    ghostty_action_s  action;
    /* Deep-copied strings (freed in idle handler) */
    char *str1;   /* url / title / pwd / body / needle */
    char *str2;   /* notification title */
} ActionIdleData;

static void action_idle_data_free(ActionIdleData *d) {
    g_free(d->str1);
    g_free(d->str2);
    g_free(d);
}

static gboolean action_idle_handler(gpointer user_data);

static bool action_cb(ghostty_app_t app, ghostty_target_s target,
                       ghostty_action_s action)
{
    (void)app;

    ghostty_surface_t surface = NULL;
    if (target.tag == GHOSTTY_TARGET_SURFACE)
        surface = target.target.surface;

    ActionIdleData *d = g_new0(ActionIdleData, 1);
    d->surface = surface;
    d->action  = action;   /* shallow copy of the struct */

    /* Deep-copy any strings the action references, then patch the
     * pointers inside the copy so the idle handler sees valid data. */
    switch (action.tag) {
    case GHOSTTY_ACTION_OPEN_URL:
        d->str1 = g_strdup(action.action.open_url.url);
        d->action.action.open_url.url = d->str1;
        break;
    case GHOSTTY_ACTION_DESKTOP_NOTIFICATION:
        d->str1 = g_strdup(action.action.desktop_notification.body);
        d->str2 = g_strdup(action.action.desktop_notification.title);
        d->action.action.desktop_notification.body  = d->str1;
        d->action.action.desktop_notification.title = d->str2;
        break;
    case GHOSTTY_ACTION_SET_TITLE:
        d->str1 = g_strdup(action.action.set_title.title);
        d->action.action.set_title.title = d->str1;
        break;
    case GHOSTTY_ACTION_PWD:
        d->str1 = g_strdup(action.action.pwd.pwd);
        d->action.action.pwd.pwd = d->str1;
        break;
    case GHOSTTY_ACTION_START_SEARCH:
        d->str1 = g_strdup(action.action.start_search.needle);
        d->action.action.start_search.needle = d->str1;
        break;
    default:
        break;
    }

    g_idle_add(action_idle_handler, d);
    return true;
}

static gboolean action_idle_handler(gpointer user_data)
{
    ActionIdleData *d = user_data;
    ghostty_surface_t surface = d->surface;
    ghostty_action_s action = d->action;

    switch (action.tag) {

    case GHOSTTY_ACTION_OPEN_URL:
        if (action.action.open_url.url) {
            add_browser_tab(action.action.open_url.url);
            gtk_widget_set_visible(ui.browser_notebook, TRUE);
        }
        break;

    case GHOSTTY_ACTION_DESKTOP_NOTIFICATION: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        send_desktop_notification("PrettyMux",
                                  action.action.desktop_notification.body,
                                  loc.workspace_idx,
                                  loc.pane_idx,
                                  loc.tab_idx);
        break;
    }

    case GHOSTTY_ACTION_SET_TITLE: {
        if (!action.action.set_title.title ||
            !action.action.set_title.title[0])
            break;
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_set_title(loc.terminal,
                                       action.action.set_title.title);
            /* Also update workspace title + sidebar label */
            if (loc.workspace) {
                snprintf(loc.workspace->name, sizeof(loc.workspace->name),
                         "%.60s", action.action.set_title.title);
                workspace_refresh_sidebar_label(loc.workspace);
            }
        }
        break;
    }

    case GHOSTTY_ACTION_PWD: {
        if (!action.action.pwd.pwd ||
            !action.action.pwd.pwd[0])
            break;
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_set_cwd(loc.terminal,
                                     action.action.pwd.pwd);
            if (loc.workspace) {
                snprintf(loc.workspace->cwd, sizeof(loc.workspace->cwd),
                         "%s", action.action.pwd.pwd);
                workspace_detect_git(loc.workspace);
                /* Update the terminal status bar with CWD + git branch */
                ghostty_terminal_set_status(loc.terminal,
                                            action.action.pwd.pwd,
                                            loc.workspace->git_branch);
            }
        }
        break;
    }

    case GHOSTTY_ACTION_COMMAND_FINISHED: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_notify_command_finished(
                loc.terminal,
                action.action.command_finished.exit_code,
                action.action.command_finished.duration);

            /* Update sidebar notification for finished commands */
            double secs = action.action.command_finished.duration / 1000000000.0;
            if (secs > 3.0 && loc.workspace) {
                if (action.action.command_finished.exit_code == 0)
                    snprintf(loc.workspace->notification,
                             sizeof(loc.workspace->notification),
                             "Command done (%.1fs)", secs);
                else
                    snprintf(loc.workspace->notification,
                             sizeof(loc.workspace->notification),
                             "Exit %d (%.1fs)",
                             action.action.command_finished.exit_code, secs);
                workspace_refresh_sidebar_label(loc.workspace);
            }

            /* Desktop notification for long-running commands (>3s) */
            if (secs > 3.0) {
                char body[256];
                const char *ws_name = loc.workspace ? loc.workspace->name : "?";
                const char *term_title = ghostty_terminal_get_title(loc.terminal);
                if (!term_title || !term_title[0])
                    term_title = "Terminal";

                if (action.action.command_finished.exit_code == 0)
                    snprintf(body, sizeof(body),
                             "Command finished in %s/%s (%.1fs)",
                             ws_name, term_title, secs);
                else
                    snprintf(body, sizeof(body),
                             "Command failed (exit %d) in %s/%s (%.1fs)",
                             action.action.command_finished.exit_code,
                             ws_name, term_title, secs);

                /* Add to in-app notification system */
                {
                    char notif_msg[256];
                    snprintf(notif_msg, sizeof(notif_msg),
                             "Command finished in %s/%s",
                             ws_name, term_title);
                    notifications_add_full(notif_msg,
                                           loc.workspace_idx,
                                           loc.pane_notebook,
                                           loc.tab_idx);
                    bell_button_update();
                    workspace_mark_tab_notification(loc.pane_notebook,
                                                    loc.tab_idx);
                    if (g_main_window_active) {
                        debug_notification_log(
                            "notify route command toast active=%d target=(%d,%d,%d) msg=%s",
                            g_main_window_active,
                            loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                            notif_msg);
                        sidebar_toast_show(notif_msg,
                                           loc.workspace_idx,
                                           loc.pane_notebook,
                                           loc.tab_idx);
                    } else {
                        debug_notification_log(
                            "notify route command desktop active=%d target=(%d,%d,%d) msg=%s",
                            g_main_window_active,
                            loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                            body);
                        send_desktop_notification("PrettyMux", body,
                                                  loc.workspace_idx,
                                                  loc.pane_idx,
                                                  loc.tab_idx);
                    }
                }
            }
        }
        break;
    }

    case GHOSTTY_ACTION_RING_BELL: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        char body[256];
        if (loc.terminal)
            ghostty_terminal_notify_bell(loc.terminal);

        /* Add to notification system with navigation info */
        {
            const char *ws_name = loc.workspace ? loc.workspace->name : "?";
            const char *term_title = loc.terminal
                ? ghostty_terminal_get_title(loc.terminal) : NULL;
            if (!term_title || !term_title[0])
                term_title = "Terminal";
            char notif_msg[256];
            snprintf(notif_msg, sizeof(notif_msg), "Bell in %s/%s",
                     ws_name, term_title);
            notifications_add_full(notif_msg,
                                   loc.workspace_idx,
                                   loc.pane_notebook,
                                   loc.tab_idx);
            bell_button_update();
            workspace_mark_tab_notification(loc.pane_notebook, loc.tab_idx);
        }

        if (!notification_target_is_active(loc.workspace_idx,
                                           loc.pane_notebook,
                                           loc.tab_idx)) {
            const char *ws_name = loc.workspace ? loc.workspace->name : "?";
            const char *term_title = loc.terminal
                ? ghostty_terminal_get_title(loc.terminal) : NULL;
            if (!term_title || !term_title[0])
                term_title = "Terminal";

            snprintf(body, sizeof(body), "Bell in %s/%s", ws_name, term_title);

            if (g_main_window_active) {
                debug_notification_log(
                    "notify route bell toast active=%d target=(%d,%d,%d) msg=%s",
                    g_main_window_active,
                    loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                    body);
                sidebar_toast_show(body,
                                   loc.workspace_idx,
                                   loc.pane_notebook,
                                   loc.tab_idx);
            } else {
                debug_notification_log(
                    "notify route bell desktop active=%d target=(%d,%d,%d) msg=%s",
                    g_main_window_active,
                    loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                    body);
                send_desktop_notification("PrettyMux", body,
                                          loc.workspace_idx,
                                          loc.pane_idx,
                                          loc.tab_idx);
            }
        } else {
            debug_notification_log(
                "notify route bell suppressed active-target target=(%d,%d,%d) msg=%s",
                loc.workspace_idx, loc.pane_idx, loc.tab_idx,
                body);
        }
        break;
    }

    case GHOSTTY_ACTION_RENDER: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_queue_render(loc.terminal);

            /* Mark activity if this terminal is NOT on the currently visible
             * tab of the currently visible workspace */
            if (loc.workspace_idx >= 0 &&
                loc.workspace_idx != current_workspace) {
                ghostty_terminal_mark_activity(loc.terminal);
                workspace_refresh_sidebar_label(loc.workspace);
            } else if (loc.workspace) {
                /* Same workspace, but check if it is the active tab */
                GtkNotebook *focused = workspace_get_focused_pane(loc.workspace);
                if (focused) {
                    int pg = gtk_notebook_get_current_page(focused);
                    GtkWidget *visible_terminal = (pg >= 0)
                        ? GTK_WIDGET(notebook_terminal_at(focused, pg))
                        : NULL;
                    if (visible_terminal != GTK_WIDGET(loc.terminal)) {
                        ghostty_terminal_mark_activity(loc.terminal);
                        workspace_refresh_tab_labels(loc.workspace);
                    }
                }
            }
        }
        break;
    }

    case GHOSTTY_ACTION_SHOW_CHILD_EXITED: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal) {
            ghostty_terminal_notify_child_exited(
                loc.terminal,
                action.action.child_exited.exit_code);
        }
        break;
    }

    case GHOSTTY_ACTION_PROGRESS_REPORT: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal && loc.workspace) {
            int pct = (int)action.action.progress_report.progress;
            int state = (int)action.action.progress_report.state;
            if (pct >= 0)
                snprintf(loc.workspace->notification,
                         sizeof(loc.workspace->notification),
                         "Progress: %d%%", pct);
            else
                loc.workspace->notification[0] = '\0';

            /* Store progress on the terminal and refresh tab labels */
            ghostty_terminal_set_progress(loc.terminal, state, pct);
            workspace_refresh_tab_labels(loc.workspace);
        }
        break;
    }

    case GHOSTTY_ACTION_START_SEARCH: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal)
            terminal_search_show(loc.terminal, action.action.start_search.needle);
        break;
    }

    case GHOSTTY_ACTION_SEARCH_TOTAL: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal == g_terminal_search_target) {
            g_terminal_search_total = action.action.search_total.total;
            terminal_search_update_count_label();
        }
        break;
    }

    case GHOSTTY_ACTION_SEARCH_SELECTED: {
        SurfaceLookup loc = find_terminal_for_surface(surface);
        if (loc.terminal == g_terminal_search_target) {
            g_terminal_search_selected = action.action.search_selected.selected;
            terminal_search_update_count_label();
        }
        break;
    }

    default:
        break;
    }

    action_idle_data_free(d);
    return G_SOURCE_REMOVE;
}

// ── Sidebar ──

static void on_workspace_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer d) {
    (void)list; (void)d;
    workspace_switch(gtk_list_box_row_get_index(row), ui.terminal_stack, ui.workspace_list);
    session_queue_save();
}

// ── Build window ──

static void on_add_workspace_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
}

static void build_sidebar(void) {
    ui.sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(ui.sidebar_box, "sidebar");
    gtk_widget_set_size_request(ui.sidebar_box, 180, -1);

    // Search
    GtkWidget *search = gtk_search_entry_new();
    gtk_widget_set_margin_start(search, 8);
    gtk_widget_set_margin_end(search, 8);
    gtk_widget_set_margin_top(search, 8);
    gtk_widget_set_margin_bottom(search, 4);
    gtk_box_append(GTK_BOX(ui.sidebar_box), search);

    // Workspace list
    ui.workspace_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui.workspace_list), GTK_SELECTION_SINGLE);
    g_signal_connect(ui.workspace_list, "row-activated", G_CALLBACK(on_workspace_row_activated), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui.workspace_list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(ui.sidebar_box), scroll);

    // Bottom bar: bell button + add workspace button side by side
    GtkWidget *bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(bottom_box, 8);
    gtk_widget_set_margin_end(bottom_box, 8);
    gtk_widget_set_margin_bottom(bottom_box, 8);
    gtk_widget_set_margin_top(bottom_box, 4);

    // Bell / notification button
    ui.bell_button = gtk_button_new_with_label("\360\237\224\224");   /* bell emoji */
    g_signal_connect(ui.bell_button, "clicked",
                     G_CALLBACK(on_bell_button_clicked), NULL);
    gtk_box_append(GTK_BOX(bottom_box), ui.bell_button);

    // Add workspace button
    GtkWidget *btn = gtk_button_new_with_label("+ New Workspace");
    gtk_widget_set_hexpand(btn, TRUE);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_add_workspace_clicked), NULL);
    gtk_box_append(GTK_BOX(bottom_box), btn);

    gtk_box_append(GTK_BOX(ui.sidebar_box), bottom_box);
}

static void
build_toast_overlay(void)
{
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

    GtkWidget *toast_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(toast_box, 10);
    gtk_widget_set_margin_end(toast_box, 10);
    gtk_widget_set_margin_top(toast_box, 8);
    gtk_widget_set_margin_bottom(toast_box, 8);
    gtk_frame_set_child(GTK_FRAME(ui.toast_frame), toast_box);

    GtkWidget *toast_button = gtk_button_new();
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

    GtkWidget *toast_close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(toast_close, "flat");
    g_signal_connect(toast_close, "clicked",
                     G_CALLBACK(on_sidebar_toast_close_clicked), NULL);
    gtk_box_append(GTK_BOX(toast_box), toast_close);

    gtk_overlay_add_overlay(GTK_OVERLAY(ui.overlay), ui.toast_revealer);
    apply_toast_position_setting();
}

static void on_new_browser_tab_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    add_browser_tab("https://prettymux-web.vercel.app/?prettymux=t");
    session_queue_save();
}

static void build_browser(void) {
    ui.browser_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(ui.browser_notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(ui.browser_notebook), FALSE);

    GtkWidget *btn = gtk_button_new_with_label("+");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_new_browser_tab_clicked), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(ui.browser_notebook), btn, GTK_PACK_END);
    gtk_widget_set_visible(btn, TRUE);
}

static gboolean autosave_tick(gpointer d) {
    (void)d;
    session_save(g_main_window, ui.browser_notebook, ui.terminal_stack, ui.workspace_list);
    return G_SOURCE_CONTINUE;
}

/* Save session immediately — call after any state-modifying action */
static void save_session_now(void) {
    if (g_main_window)
        session_save(g_main_window, ui.browser_notebook,
                     ui.terminal_stack, ui.workspace_list);
}

typedef struct {
    Workspace *ws;
    GtkNotebook *pane;
} PendingPaneClose;

typedef struct {
    int index;
} PendingWorkspaceClose;

typedef struct {
    GtkNotebook *notebook;
    int page;
} PendingBrowserTabClose;

typedef struct {
    Workspace *ws;
    GtkNotebook *notebook;
    int page;
} PendingMainTabClose;

static void
pending_pane_close_free(gpointer data)
{
    PendingPaneClose *pending = data;

    if (pending->pane)
        g_object_unref(pending->pane);
    g_free(pending);
}

static void
pending_workspace_close_free(gpointer data)
{
    g_free(data);
}

static void
pending_browser_tab_close_free(gpointer data)
{
    PendingBrowserTabClose *pending = data;

    if (pending->notebook)
        g_object_unref(pending->notebook);
    g_free(pending);
}

static void
pending_main_tab_close_free(gpointer data)
{
    PendingMainTabClose *pending = data;

    if (pending->notebook)
        g_object_unref(pending->notebook);
    g_free(pending);
}

static void
perform_app_quit(void)
{
    if (g_app_quit_in_progress)
        return;

    save_session_now();
    g_app_quit_in_progress = TRUE;
    session_begin_shutdown();
    workspace_set_shutting_down();
    socket_server_stop();

    GApplication *app = g_application_get_default();
    if (app)
        g_application_quit(app);
}

static gboolean
quit_window_idle_cb(gpointer data)
{
    (void)data;
    perform_app_quit();
    return G_SOURCE_REMOVE;
}

static void
on_app_close_confirmed(gboolean confirmed, gpointer user_data)
{
    (void)user_data;
    if (confirmed)
        perform_app_quit();
}

static void
request_app_quit(void)
{
    if (g_app_quit_in_progress)
        return;

    close_confirm_request(g_main_window, CLOSE_CONFIRM_APP,
                          on_app_close_confirmed, NULL, NULL);
}

static void
on_pane_close_confirmed(gboolean confirmed, gpointer user_data)
{
    PendingPaneClose *pending = user_data;

    if (confirmed && pending->ws && pending->pane) {
        workspace_close_pane(pending->ws, pending->pane);
        session_queue_save();
    }
}

static void
on_workspace_close_confirmed(gboolean confirmed, gpointer user_data)
{
    PendingWorkspaceClose *pending = user_data;

    if (confirmed && pending->index >= 0) {
        workspace_remove(pending->index, ui.terminal_stack, ui.workspace_list);
        session_queue_save();
    }
}

static void
request_close_current_workspace(void)
{
    PendingWorkspaceClose *pending;

    if (!workspaces || workspaces->len <= 1 || current_workspace < 0 ||
        current_workspace >= (int)workspaces->len)
        return;

    pending = g_new0(PendingWorkspaceClose, 1);
    pending->index = current_workspace;
    close_confirm_request(g_main_window, CLOSE_CONFIRM_WORKSPACE,
                          on_workspace_close_confirmed, pending,
                          pending_workspace_close_free);
}

static void
request_close_current_pane(Workspace *ws)
{
    GtkNotebook *focused;
    PendingPaneClose *pending;

    if (!ws)
        return;

    focused = workspace_get_focused_pane(ws);
    if (!focused || !ws->pane_notebooks || ws->pane_notebooks->len <= 1)
        return;

    pending = g_new0(PendingPaneClose, 1);
    pending->ws = ws;
    pending->pane = g_object_ref(focused);
    close_confirm_request(g_main_window, CLOSE_CONFIRM_PANE,
                          on_pane_close_confirmed, pending,
                          pending_pane_close_free);
}

static void
on_tab_close_confirmed(gboolean confirmed, gpointer user_data)
{
    PendingMainTabClose *pending = user_data;

    if (confirmed && pending->ws && pending->notebook)
        workspace_close_tab_at(pending->ws, pending->notebook, pending->page);
}

static void
request_close_current_tab(Workspace *ws)
{
    GtkNotebook *focused;
    int n_pages;
    int pg;
    PendingMainTabClose *pending;

    if (!ws)
        return;

    focused = workspace_get_focused_pane(ws);
    if (!focused)
        return;

    n_pages = gtk_notebook_get_n_pages(focused);
    pg = gtk_notebook_get_current_page(focused);
    if (pg < 0)
        return;
    if (n_pages <= 1 &&
        (!ws->pane_notebooks || ws->pane_notebooks->len <= 1))
        return;

    pending = g_new0(PendingMainTabClose, 1);
    pending->ws = ws;
    pending->notebook = g_object_ref(focused);
    pending->page = pg;
    close_confirm_request(g_main_window, CLOSE_CONFIRM_TAB,
                          on_tab_close_confirmed, pending,
                          pending_main_tab_close_free);
}

static void
on_browser_tab_close_confirmed(gboolean confirmed, gpointer user_data)
{
    PendingBrowserTabClose *pending = user_data;

    if (!confirmed || !pending->notebook)
        return;

    if (pending->page >= 0 &&
        pending->page < gtk_notebook_get_n_pages(pending->notebook)) {
        gtk_notebook_remove_page(pending->notebook, pending->page);
        session_queue_save();
    }
}

static void
request_close_current_browser_tab(void)
{
    GtkNotebook *notebook = GTK_NOTEBOOK(ui.browser_notebook);
    int n;
    int pg;
    PendingBrowserTabClose *pending;

    if (!gtk_widget_get_visible(ui.browser_notebook))
        return;

    n = gtk_notebook_get_n_pages(notebook);
    if (n <= 1)
        return;

    pg = gtk_notebook_get_current_page(notebook);
    if (pg < 0)
        return;

    pending = g_new0(PendingBrowserTabClose, 1);
    pending->notebook = g_object_ref(notebook);
    pending->page = pg;
    close_confirm_request(g_main_window, CLOSE_CONFIRM_TAB,
                          on_browser_tab_close_confirmed, pending,
                          pending_browser_tab_close_free);
}

static gboolean on_close_request(GtkWindow *w, gpointer d) {
    (void)w; (void)d;
    if (g_app_quit_in_progress)
        return FALSE;

    request_app_quit();
    return TRUE;
}

static void
on_paned_position_notify(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object;
    (void)pspec;
    (void)user_data;
    session_queue_save();
}

static void
on_app_shutdown(GApplication *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    port_scanner_stop();
    socket_server_stop();
    if (!g_app_quit_in_progress) {
        session_begin_shutdown();
        workspace_set_shutting_down();
    }
}

static gboolean
on_unix_quit_signal(gpointer user_data)
{
    (void)user_data;
    perform_app_quit();
    return G_SOURCE_CONTINUE;
}

// ── Feature 3: Welcome Dialog ──

static void
on_welcome_ok_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    GtkWindow *dlg = GTK_WINDOW(user_data);

    /* Check if the "don't show again" checkbox is checked */
    GtkWidget *check = g_object_get_data(G_OBJECT(dlg), "check-button");
    if (check && gtk_check_button_get_active(GTK_CHECK_BUTTON(check))) {
        char *config_dir = g_build_filename(g_get_home_dir(),
                                            ".config", "prettymux", NULL);
        g_mkdir_with_parents(config_dir, 0755);
        char *flag_path = g_build_filename(config_dir, ".welcome-shown", NULL);
        g_file_set_contents(flag_path, "1", 1, NULL);
        g_free(flag_path);
        g_free(config_dir);
    }

    gtk_window_destroy(dlg);
}

static void
show_welcome_dialog(GtkWindow *parent)
{
    char *flag_path = g_build_filename(g_get_home_dir(),
                                       ".config", "prettymux",
                                       ".welcome-shown", NULL);
    gboolean already_shown = g_file_test(flag_path, G_FILE_TEST_EXISTS);
    g_free(flag_path);

    if (already_shown)
        return;

    GtkWindow *dlg = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(dlg, "Welcome to PrettyMux");
    gtk_window_set_default_size(dlg, 420, 340);
    gtk_window_set_resizable(dlg, FALSE);
    gtk_window_set_modal(dlg, TRUE);
    gtk_window_set_transient_for(dlg, parent);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 32);
    gtk_widget_set_margin_end(box, 32);
    gtk_widget_set_margin_top(box, 28);
    gtk_widget_set_margin_bottom(box, 24);

    /* Title */
    GtkWidget *title = gtk_label_new("Welcome to PrettyMux");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5f);
    gtk_box_append(GTK_BOX(box), title);

    /* Description */
    GtkWidget *desc = gtk_label_new(
        "GPU-accelerated terminal multiplexer with\n"
        "ghostty + WebKit in one window.\n\n"
        "Press Ctrl+Shift+K to see all shortcuts.\n"
        "Visit prettymux-web.vercel.app for docs.");
    gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(desc), 0.5f);
    gtk_label_set_justify(GTK_LABEL(desc), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(box), desc);

    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(box), spacer);

    /* Checkbox */
    GtkWidget *check = gtk_check_button_new_with_label("Don't show this again");
    gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), check);

    /* OK button */
    GtkWidget *ok_btn = gtk_button_new_with_label("Get Started");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_widget_set_halign(ok_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(ok_btn, 140, -1);
    gtk_box_append(GTK_BOX(box), ok_btn);

    /* Store checkbox reference on the dialog for the callback */
    g_object_set_data(G_OBJECT(dlg), "check-button", check);

    g_signal_connect(ok_btn, "clicked",
                     G_CALLBACK(on_welcome_ok_clicked), dlg);

    gtk_window_set_child(dlg, box);
    gtk_window_present(dlg);
}

/* ── GApplication action: navigate to a specific terminal from notification click ── */

static void
on_navigate_to_terminal(GSimpleAction *action_obj, GVariant *parameter,
                        gpointer user_data)
{
    (void)action_obj;
    (void)user_data;

    if (!parameter)
        return;

    int ws_idx = 0, pane_idx = 0, tab_idx = 0;
    g_variant_get(parameter, "(iii)", &ws_idx, &pane_idx, &tab_idx);

    /* Switch to the workspace */
    if (ws_idx >= 0 && workspaces && ws_idx < (int)workspaces->len) {
        workspace_switch(ws_idx, ui.terminal_stack, ui.workspace_list);

        Workspace *ws = g_ptr_array_index(workspaces, ws_idx);
        /* Find the pane notebook (use pane_idx or fallback to first) */
        GtkNotebook *nb = NULL;
        if (ws->pane_notebooks && pane_idx >= 0 &&
            pane_idx < (int)ws->pane_notebooks->len)
            nb = g_ptr_array_index(ws->pane_notebooks, pane_idx);
        else if (ws->pane_notebooks && ws->pane_notebooks->len > 0)
            nb = g_ptr_array_index(ws->pane_notebooks, 0);

        if (nb && tab_idx >= 0 &&
            tab_idx < gtk_notebook_get_n_pages(nb)) {
            gtk_notebook_set_current_page(nb, tab_idx);
            GhosttyTerminal *term = notebook_terminal_at(nb, tab_idx);
            if (term)
                ghostty_terminal_focus(term);
        }
    }

    /* Bring window to front */
    if (g_main_window)
        gtk_window_present(g_main_window);
}

static void
setup_shell_integration_env(void)
{
#ifdef G_OS_WIN32
    wchar_t exe_path_w[PATH_MAX];
    DWORD exe_len = GetModuleFileNameW(NULL, exe_path_w, G_N_ELEMENTS(exe_path_w));
    g_autofree char *exe_path = NULL;
#else
    char exe_path[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
#endif

    if (exe_len <= 0)
        return;

#ifdef G_OS_WIN32
    exe_path = g_utf16_to_utf8((const gunichar2 *)exe_path_w, exe_len, NULL, NULL, NULL);
    if (!exe_path)
        return;
#else
    exe_path[exe_len] = '\0';
#endif

    char *exe_dir_buf = g_path_get_dirname(
#ifdef G_OS_WIN32
        exe_path
#else
        exe_path
#endif
    );
    const char *exe_dir = exe_dir_buf;
    char *open_cli = g_build_filename(exe_dir, "prettymux-open", NULL);
    char *shell_integ = g_build_filename(exe_dir, "prettymux-shell-integration.sh", NULL);
    char *bashrc_wrapper = g_build_filename(exe_dir, "prettymux-bashrc.sh", NULL);
    char *ghostty_bash = g_build_filename(exe_dir, "ghostty.bash", NULL);
    char *wrapper_dir = g_build_filename(exe_dir, "bin", NULL);

    if (!g_file_test(shell_integ, G_FILE_TEST_EXISTS)) {
        g_free(shell_integ);
        shell_integ = g_build_filename(PRETTYMUX_SOURCE_DIR,
                                       "prettymux-shell-integration.sh", NULL);
    }

    if (!g_file_test(shell_integ, G_FILE_TEST_EXISTS)) {
        g_free(shell_integ);
        shell_integ = g_strdup("/app/share/prettymux/shell-integration.sh");
    }

    if (!g_file_test(shell_integ, G_FILE_TEST_EXISTS)) {
        g_free(shell_integ);
        shell_integ = g_strdup("/usr/share/prettymux/shell-integration.sh");
    }

    if (!g_file_test(open_cli, G_FILE_TEST_EXISTS)) {
        g_free(open_cli);
        open_cli = g_find_program_in_path("prettymux-open");
    }

    if (!g_file_test(ghostty_bash, G_FILE_TEST_EXISTS)) {
        g_free(ghostty_bash);
        ghostty_bash = g_build_filename(PRETTYMUX_SOURCE_DIR,
                                        "..", "..", "..",
                                        "ghostty", "src", "shell-integration",
                                        "bash", "ghostty.bash", NULL);
    }

    if (!g_file_test(ghostty_bash, G_FILE_TEST_EXISTS)) {
        g_free(ghostty_bash);
        ghostty_bash = g_strdup("/app/share/ghostty/shell-integration/bash/ghostty.bash");
    }

    if (!g_file_test(ghostty_bash, G_FILE_TEST_EXISTS)) {
        g_free(ghostty_bash);
        ghostty_bash = g_strdup("/usr/share/ghostty/shell-integration/bash/ghostty.bash");
    }

    if (!g_file_test(bashrc_wrapper, G_FILE_TEST_EXISTS)) {
        g_free(bashrc_wrapper);
        bashrc_wrapper = g_build_filename(PRETTYMUX_SOURCE_DIR,
                                          "prettymux-bashrc.sh", NULL);
    }

    if (!g_file_test(bashrc_wrapper, G_FILE_TEST_EXISTS)) {
        g_free(bashrc_wrapper);
        bashrc_wrapper = g_strdup("/app/share/prettymux/prettymux-bashrc.sh");
    }

    if (!g_file_test(bashrc_wrapper, G_FILE_TEST_EXISTS)) {
        g_free(bashrc_wrapper);
        bashrc_wrapper = g_strdup("/usr/share/prettymux/prettymux-bashrc.sh");
    }

    if (!g_file_test(wrapper_dir, G_FILE_TEST_IS_DIR)) {
        g_free(wrapper_dir);
        wrapper_dir = g_strdup("/app/share/prettymux/bin");
    }

    if (!g_file_test(wrapper_dir, G_FILE_TEST_IS_DIR)) {
        g_free(wrapper_dir);
        wrapper_dir = g_strdup("/usr/share/prettymux/bin");
    }

    if (!g_file_test(wrapper_dir, G_FILE_TEST_IS_DIR)) {
        g_free(wrapper_dir);
        wrapper_dir = g_build_filename(PRETTYMUX_SOURCE_DIR, "bin", NULL);
    }

    if (g_file_test(shell_integ, G_FILE_TEST_EXISTS))
        g_setenv("BASH_ENV", shell_integ, TRUE);

    if (g_file_test(shell_integ, G_FILE_TEST_EXISTS))
        g_setenv("PRETTYMUX_SHELL_INTEGRATION", shell_integ, TRUE);

    if (g_file_test(bashrc_wrapper, G_FILE_TEST_EXISTS))
        g_setenv("GHOSTTY_BASH_RCFILE", bashrc_wrapper, TRUE);

    if (open_cli && g_file_test(open_cli, G_FILE_TEST_EXISTS))
        g_setenv("PRETTYMUX_OPEN_BIN", open_cli, TRUE);

    if (ghostty_bash && g_file_test(ghostty_bash, G_FILE_TEST_EXISTS))
        g_setenv("PRETTYMUX_GHOSTTY_BASH_INTEGRATION", ghostty_bash, TRUE);

    if (wrapper_dir && g_file_test(wrapper_dir, G_FILE_TEST_IS_DIR)) {
        const char *old_path = g_getenv("PATH");
        if (!old_path || !old_path[0]) {
            g_setenv("PATH", wrapper_dir, TRUE);
        } else {
            size_t wrapper_len = strlen(wrapper_dir);
            gboolean already_prefixed =
                g_str_has_prefix(old_path, wrapper_dir) &&
                (old_path[wrapper_len] == '\0' || old_path[wrapper_len] == ':');
            if (!already_prefixed) {
                char *new_path = g_strdup_printf("%s:%s", wrapper_dir, old_path);
                g_setenv("PATH", new_path, TRUE);
                g_free(new_path);
            }
        }
    }

    g_free(open_cli);
    g_free(ghostty_bash);
    g_free(shell_integ);
    g_free(bashrc_wrapper);
    g_free(wrapper_dir);
    g_free(exe_dir_buf);
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    if (g_main_window && GTK_IS_WINDOW(g_main_window)) {
        gtk_window_present(g_main_window);
        return;
    }

    debug_notification_log("notify app activate app_id=%s registered=%d dbus=%p",
                           g_application_get_application_id(G_APPLICATION(app))
                               ? g_application_get_application_id(G_APPLICATION(app))
                               : "(null)",
                           g_application_get_is_registered(G_APPLICATION(app)),
                           g_application_get_dbus_connection(G_APPLICATION(app)));
    app_settings_load();
    theme_set_custom(app_settings_get_custom_theme());
    app_settings_write_ghostty_override();

    // Init ghostty
    if (ghostty_init(0, NULL) != 0) { fprintf(stderr, "ghostty_init failed\n"); return; }

    ghostty_config_t config = load_ghostty_config_with_overrides();
    g_runtime_ghostty_config = config;

    ghostty_runtime_config_s rc = {0};
    rc.wakeup_cb = wakeup_cb;
    rc.action_cb = action_cb;
    rc.read_clipboard_cb = read_clipboard_cb;
    rc.confirm_read_clipboard_cb = confirm_read_clipboard_cb;
    rc.write_clipboard_cb = write_clipboard_cb;
    rc.close_surface_cb = close_surface_cb;

    g_ghostty_app = ghostty_app_new(&rc, config);
    if (!g_ghostty_app) { fprintf(stderr, "ghostty_app_new failed\n"); return; }

    // Register GApplication action for notification click navigation
    {
        GSimpleAction *nav_action = g_simple_action_new(
            "navigate-to-terminal",
            G_VARIANT_TYPE("(iii)"));
        g_signal_connect(nav_action, "activate",
                         G_CALLBACK(on_navigate_to_terminal), NULL);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(nav_action));
        g_object_unref(nav_action);
    }

    // Theme
    theme_apply();
    apply_theme_to_ghostty_scheme();

    // Window
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "PrettyMux");
    gtk_window_set_default_icon_name(prettymux_icon_name());
    gtk_window_set_icon_name(GTK_WINDOW(window), prettymux_icon_name());
    gtk_window_set_default_size(GTK_WINDOW(window), 1400, 900);
    g_main_window = GTK_WINDOW(window);
    g_main_window_active = gtk_window_is_active(GTK_WINDOW(window));
    hover_focus_handle_window_active_changed(g_main_window_active);
    g_signal_connect(window, "notify::is-active",
                     G_CALLBACK(on_main_window_active_changed), NULL);

    // Shortcut handler (capture phase)
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    gtk_widget_add_controller(window, kc);

    // Layout: overlay wraps everything so the command palette can float
    ui.overlay = gtk_overlay_new();
    gtk_window_set_child(GTK_WINDOW(window), ui.overlay);

    ui.outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_overlay_set_child(GTK_OVERLAY(ui.overlay), ui.outer_paned);
    build_toast_overlay();

    build_sidebar();
    gtk_paned_set_start_child(GTK_PANED(ui.outer_paned), ui.sidebar_box);
    gtk_paned_set_resize_start_child(GTK_PANED(ui.outer_paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(ui.outer_paned), FALSE);

    ui.main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_end_child(GTK_PANED(ui.outer_paned), ui.main_paned);

    // Terminal area: vertical box holding terminal_stack + notes panel
    ui.terminal_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(ui.terminal_box, TRUE);
    gtk_widget_set_vexpand(ui.terminal_box, TRUE);

    ui.terminal_stack = gtk_stack_new();
    gtk_widget_set_hexpand(ui.terminal_stack, TRUE);
    gtk_widget_set_vexpand(ui.terminal_stack, TRUE);
    gtk_box_append(GTK_BOX(ui.terminal_box), ui.terminal_stack);

    gtk_paned_set_start_child(GTK_PANED(ui.main_paned), ui.terminal_box);

    build_browser();
    gtk_paned_set_end_child(GTK_PANED(ui.main_paned), ui.browser_notebook);

    gtk_paned_set_position(GTK_PANED(ui.outer_paned), 200);
    gtk_paned_set_position(GTK_PANED(ui.main_paned), 700);

    // Resize overlay — show dimensions when paned handles are dragged
    resize_overlay_init(GTK_OVERLAY(ui.overlay));
    resize_overlay_connect_paned(GTK_PANED(ui.outer_paned));
    resize_overlay_connect_paned(GTK_PANED(ui.main_paned));
    g_signal_connect(ui.outer_paned, "notify::position",
                     G_CALLBACK(on_paned_position_notify), NULL);
    g_signal_connect(ui.main_paned, "notify::position",
                     G_CALLBACK(on_paned_position_notify), NULL);

    // Command palette (overlay)
    ui.command_palette = command_palette_new(ui.browser_notebook,
                                             ui.terminal_stack,
                                             ui.workspace_list);
    gtk_widget_set_visible(ui.command_palette, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(ui.overlay), ui.command_palette);

    session_set_context(GTK_WINDOW(window), ui.browser_notebook,
                        ui.terminal_stack, ui.workspace_list);

    // Save on close
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), NULL);

    // ── Single instance socket server ──
    socket_server_set_callback(on_socket_command, NULL);
    const char *sock_path = socket_server_start();
    if (sock_path) {
        setup_shell_integration_env();
    }
    port_scanner_set_callback(on_port_scanner_detected, NULL);
    port_scanner_set_window_active(g_main_window_active);
    port_scanner_set_active_workspace(current_workspace);
    port_scanner_start();

    // Create initial workspace + restore or create defaults
    workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);

    if (session_exists()) {
        session_restore(GTK_WINDOW(window), ui.browser_notebook,
                        ui.terminal_stack, ui.workspace_list, g_ghostty_app,
                        add_browser_tab);
        apply_theme_to_ghostty_scheme();
    }
    // Always ensure at least one browser tab exists
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui.browser_notebook)) == 0) {
        add_browser_tab("https://prettymux-web.vercel.app/?prettymux=t");
    }

    // Auto-save
    g_timeout_add_seconds(30, autosave_tick, NULL);

    gtk_window_present(GTK_WINDOW(window));

    // ── Welcome dialog (first run) ──
    show_welcome_dialog(GTK_WINDOW(window));
}

// ── Entry point ──

int main(int argc, char *argv[]) {
    int cli_exit = 0;

    if (prettymux_agent_cli_maybe_run(argc, argv, &cli_exit))
        return cli_exit;

    // WebKitGTK's bubblewrap sandbox requires dbus-proxy which may not
    // be available in all environments. Disable it for now.
#ifndef G_OS_WIN32
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", FALSE);
#endif
    g_set_prgname("prettymux");
    ensure_local_desktop_entry();

    AdwApplication *app = adw_application_new("dev.prettymux.app",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    /* Save session on SIGTERM/SIGINT */
#ifndef G_OS_WIN32
    g_unix_signal_add(SIGTERM, on_unix_quit_signal, NULL);
    g_unix_signal_add(SIGINT, on_unix_quit_signal, NULL);
#endif

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    /* Let process exit reclaim libghostty state. The embedded GTK teardown
     * does not guarantee every surface is gone before app shutdown completes,
     * and forcing ghostty_app_free() here can crash on quit. */
    g_ghostty_app = NULL;
    return status;
}
