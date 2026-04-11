#define _GNU_SOURCE

#include "app_support.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "workspace.h"

#ifndef PRETTYMUX_VERSION
#define PRETTYMUX_VERSION "0.2.17"
#endif

#define PRETTYMUX_GITHUB_URL "https://github.com/patcito/prettymux"
#define PRETTYMUX_WEBSITE_URL "https://prettymux.com/"
#define PRETTYMUX_LICENSE_NAME "GPL-3.0-only"

static guint g_desktop_notification_serial = 0;
static gint64 g_recording_origin_us = -1;
static gboolean g_main_window_active = FALSE;
static GtkWindow *g_about_window = NULL;

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

void
app_support_set_main_window_active(gboolean active)
{
    g_main_window_active = active;
}

const char *
prettymux_icon_name(void)
{
    return "prettymux";
}

void
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

void
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

void
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

    icon = g_themed_icon_new("prettymux");
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

void
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

static void
on_welcome_ok_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    GtkWindow *dlg = GTK_WINDOW(user_data);
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

void
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

    GtkWidget *title = gtk_label_new("Welcome to PrettyMux");
    gtk_widget_add_css_class(title, "title-1");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5f);
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *desc = gtk_label_new(
        "GPU-accelerated terminal multiplexer with\n"
        "ghostty + WebKit in one window.\n\n"
        "Press Ctrl+Shift+K to see all shortcuts.\n"
        "Visit prettymux-web.vercel.app for docs.");
    gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(desc), 0.5f);
    gtk_label_set_justify(GTK_LABEL(desc), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(box), desc);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(box), spacer);

    GtkWidget *check = gtk_check_button_new_with_label("Don't show this again");
    gtk_widget_set_halign(check, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), check);

    GtkWidget *ok_btn = gtk_button_new_with_label("Get Started");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_widget_set_halign(ok_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(ok_btn, 140, -1);
    gtk_box_append(GTK_BOX(box), ok_btn);

    g_object_set_data(G_OBJECT(dlg), "check-button", check);
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_welcome_ok_clicked), dlg);

    gtk_window_set_child(dlg, box);
    gtk_window_present(dlg);
}
