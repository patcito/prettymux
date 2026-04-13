#define _GNU_SOURCE
// PrettyMux — GTK4 + WebKitGTK + ghostty (OpenGL)
// GPU-accelerated terminal multiplexer with integrated browser

#include <adwaita.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "app_actions.h"
#include "app_support.h"
#include "app_state.h"
#include "ghostty_actions.h"
#include "notifications.h"
#include "sidebar_ui.h"
#include "socket_commands.h"
#include "terminal_routing.h"

// ── Global state ──

#ifndef PRETTYMUX_VERSION
#define PRETTYMUX_VERSION "0.2.21"
#endif

static void terminal_search_send_action(GhosttyTerminal *term, const char *action);
static void terminal_search_hide(void);

static const char *g_renderer_probe_target = NULL;

static gboolean
renderer_probe_quit_cb(gpointer data)
{
    GApplication *app = G_APPLICATION(data);
    g_application_quit(app);
    g_object_unref(app);
    return G_SOURCE_REMOVE;
}

static void
on_renderer_probe_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    GtkWidget *window =
        gtk_application_window_new(GTK_APPLICATION(app));
    GtkWidget *gl_area = gtk_gl_area_new();

    gtk_window_set_default_size(GTK_WINDOW(window), 160, 120);
    gtk_gl_area_set_use_es(GTK_GL_AREA(gl_area), FALSE);
    gtk_window_set_child(GTK_WINDOW(window), gl_area);
    gtk_window_present(GTK_WINDOW(window));
    g_timeout_add(300, renderer_probe_quit_cb, g_object_ref(app));
}

static char *
current_executable_path(void)
{
#ifdef G_OS_WIN32
    wchar_t exe_path_w[PATH_MAX];
    DWORD exe_len = GetModuleFileNameW(NULL, exe_path_w, G_N_ELEMENTS(exe_path_w));
    if (exe_len <= 0)
        return NULL;
    return g_utf16_to_utf8((const gunichar2 *)exe_path_w, exe_len, NULL, NULL, NULL);
#else
    char exe_path[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_len <= 0)
        return NULL;
    exe_path[exe_len] = '\0';
    return g_strdup(exe_path);
#endif
}

static void
apply_graphics_renderer_choice(const char *renderer, gboolean force)
{
#ifndef G_OS_WIN32
    if (!renderer || !renderer[0])
        return;

    if (g_strcmp0(renderer, "opengl") == 0) {
        g_setenv("GSK_RENDERER", "opengl", force);
        g_setenv("GDK_DISABLE", "gles-api,vulkan", force);
    } else if (g_strcmp0(renderer, "vulkan") == 0) {
        g_setenv("GSK_RENDERER", "vulkan", force);
        if (force)
            g_unsetenv("GDK_DISABLE");
    } else if (g_strcmp0(renderer, "ngl") == 0) {
        g_setenv("GSK_RENDERER", "ngl", force);
        if (force)
            g_unsetenv("GDK_DISABLE");
    }
#endif
}

static gboolean
run_graphics_renderer_probe(const char *renderer)
{
#ifndef G_OS_WIN32
    gboolean ok = FALSE;
    gchar *exe_path = current_executable_path();
    gchar *argvv[2] = { NULL, NULL };
    gchar **envp = NULL;
    gint status = 1;
    GError *error = NULL;

    if (!exe_path)
        return FALSE;

    argvv[0] = exe_path;
    envp = g_get_environ();
    envp = g_environ_setenv(envp, "PRETTYMUX_RENDERER_PROBE", renderer, TRUE);
    envp = g_environ_unsetenv(envp, "GSK_RENDERER");
    envp = g_environ_unsetenv(envp, "GDK_DISABLE");
    envp = g_environ_unsetenv(envp, "PRETTYMUX_SOCKET");

    if (g_spawn_sync(NULL, argvv, envp, G_SPAWN_DEFAULT, NULL, NULL,
                     NULL, NULL, &status, &error))
        ok = g_spawn_check_wait_status(status, NULL);

    if (error)
        g_error_free(error);
    g_strfreev(envp);
    g_free(exe_path);
    return ok;
#else
    (void)renderer;
    return FALSE;
#endif
}

static void
apply_graphics_startup_workarounds(void)
{
#ifndef G_OS_WIN32
    const char *renderer_mode;
    const char *cached_result;

    if (g_renderer_probe_target && g_renderer_probe_target[0]) {
        apply_graphics_renderer_choice(g_renderer_probe_target, FALSE);
        return;
    }

    if (g_getenv("GSK_RENDERER") || g_getenv("GDK_DISABLE"))
        return;

    app_settings_load();
    renderer_mode = app_settings_get_gtk_renderer_mode();

    if (g_strcmp0(renderer_mode, "auto") == 0) {
        cached_result = app_settings_get_gtk_renderer_probe_result();
        if (g_strcmp0(cached_result, "vulkan") != 0 &&
            g_strcmp0(cached_result, "opengl") != 0 &&
            g_strcmp0(cached_result, "ngl") != 0) {
            cached_result = run_graphics_renderer_probe("vulkan")
                ? "vulkan"
                : "opengl";
            app_settings_set_gtk_renderer_probe_result(cached_result);
            app_settings_save();
        }
        apply_graphics_renderer_choice(cached_result, FALSE);
        return;
    }

    apply_graphics_renderer_choice(renderer_mode, FALSE);
#endif
}

static void
on_main_window_active_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    (void)user_data;
    g_main_window_active = gtk_window_is_active(GTK_WINDOW(object));
    app_support_set_main_window_active(g_main_window_active);
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

void
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

void
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
    notifications_apply_toast_position_setting();
    session_queue_save();
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

GhosttyTerminal *
notebook_terminal_at(GtkNotebook *notebook, int page_num)
{
    GtkWidget *terminal = page_linked_terminal(
        gtk_notebook_get_nth_page(notebook, page_num));
    return terminal ? GHOSTTY_TERMINAL(terminal) : NULL;
}

gboolean
focus_within_terminal(GhosttyTerminal *term)
{
    GtkWidget *focus;

    if (!term || !g_main_window)
        return FALSE;

    focus = gtk_window_get_focus(GTK_WINDOW(g_main_window));
    return focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(term));
}

gboolean
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

void
terminal_search_show(GhosttyTerminal *term, const char *needle)
{
    terminal_search_set_target(term);
    if (!term)
        return;

    ghostty_terminal_set_search_active(term, TRUE, needle ? needle : "");
    terminal_search_update_count_label();
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

// ── Terminal lookup: find GhosttyTerminal by ghostty_surface_t ──

static gboolean autosave_tick(gpointer d) {
    (void)d;
    session_save(g_main_window, ui.browser_notebook, ui.terminal_stack, ui.workspace_list);
    return G_SOURCE_CONTINUE;
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
    if (!app_actions_is_quitting()) {
        session_begin_shutdown();
        workspace_set_shutting_down();
    }
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
    rc.action_cb = ghostty_actions_action_cb;
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
    g_signal_connect(kc, "key-pressed",
                     G_CALLBACK(app_actions_on_key_pressed), NULL);
    gtk_widget_add_controller(window, kc);

    // Layout: overlay wraps everything so the command palette can float
    ui.overlay = gtk_overlay_new();
    gtk_window_set_child(GTK_WINDOW(window), ui.overlay);

    ui.outer_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_overlay_set_child(GTK_OVERLAY(ui.overlay), ui.outer_paned);
    build_toast_overlay();

    sidebar_ui_build();
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

    app_actions_build_browser();
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
    g_signal_connect(window, "close-request",
                     G_CALLBACK(app_actions_on_close_request), NULL);

    // ── Single instance socket server ──
    socket_server_set_callback(socket_commands_on_socket_command, NULL);
    const char *sock_path = socket_server_start();
    if (sock_path) {
        setup_shell_integration_env();
    }
    port_scanner_set_callback(terminal_routing_on_port_scanner_detected, NULL);
    port_scanner_set_window_active(g_main_window_active);
    port_scanner_set_active_workspace(current_workspace);
    port_scanner_start();

    // Create initial workspace + restore or create defaults
    workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);

    if (session_exists()) {
        session_restore(GTK_WINDOW(window), ui.browser_notebook,
                        ui.terminal_stack, ui.workspace_list, g_ghostty_app,
                        app_actions_add_browser_tab);
        apply_theme_to_ghostty_scheme();
    }
    // Always ensure at least one browser tab exists
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(ui.browser_notebook)) == 0) {
        app_actions_add_browser_tab("https://prettymux-web.vercel.app/?prettymux=t");
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

    g_renderer_probe_target = g_getenv("PRETTYMUX_RENDERER_PROBE");

    if (!g_renderer_probe_target &&
        prettymux_agent_cli_maybe_run(argc, argv, &cli_exit))
        return cli_exit;

    // WebKitGTK's bubblewrap sandbox requires dbus-proxy which may not
    // be available in all environments. Disable it for now.
#ifndef G_OS_WIN32
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", FALSE);
#endif
    apply_graphics_startup_workarounds();
    g_set_prgname("prettymux");
    if (!g_renderer_probe_target)
        ensure_local_desktop_entry();

    if (g_renderer_probe_target && g_renderer_probe_target[0]) {
        apply_graphics_renderer_choice(g_renderer_probe_target, TRUE);
        AdwApplication *probe_app = adw_application_new("dev.prettymux.probe",
                                                        G_APPLICATION_DEFAULT_FLAGS);
        g_signal_connect(probe_app, "activate", G_CALLBACK(on_renderer_probe_activate), NULL);
        int probe_status = g_application_run(G_APPLICATION(probe_app), argc, argv);
        g_object_unref(probe_app);
        return probe_status;
    }

    AdwApplication *app = adw_application_new("dev.prettymux.app",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    /* Save session on SIGTERM/SIGINT */
#ifndef G_OS_WIN32
    g_unix_signal_add(SIGTERM, app_actions_on_unix_quit_signal, NULL);
    g_unix_signal_add(SIGINT, app_actions_on_unix_quit_signal, NULL);
#endif

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    /* Let process exit reclaim libghostty state. The embedded GTK teardown
     * does not guarantee every surface is gone before app shutdown completes,
     * and forcing ghostty_app_free() here can crash on quit. */
    g_ghostty_app = NULL;
    return status;
}
