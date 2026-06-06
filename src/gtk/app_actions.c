#include "app_actions.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "app_settings.h"
#include "app_support.h"
#include "app_state.h"
#include "close_confirm.h"
#include "command_palette.h"
#include "ghostty.h"
#include "ghostty_terminal.h"
#include "hover_focus.h"
#include "notifications.h"
#include "pane_move_overlay.h"
#include "session.h"
#include "settings_dialog.h"
#include "shortcuts.h"
#include "shortcuts_overlay.h"
#include "socket_server.h"
#include "theme.h"
#include "workspace.h"
#include "workspace_strip.h"

static gboolean g_app_quit_in_progress = FALSE;

typedef struct {
    Workspace *ws;
    GtkNotebook *pane;
} PendingPaneClose;

typedef struct {
    int index;
} PendingWorkspaceClose;

typedef struct {
    Workspace *ws;
    GtkNotebook *notebook;
    int page;
} PendingMainTabClose;

static void save_session_now(void);
static gboolean quit_window_idle_cb(gpointer data);
static void request_close_current_tab(Workspace *ws);
static void request_close_current_pane(Workspace *ws);
static void request_close_current_workspace(void);
static void on_clipboard_text_received(GObject *source,
                                       GAsyncResult *result,
                                       gpointer user_data);
static void perform_app_quit(void);
static void on_app_close_confirmed(gboolean confirmed, gpointer user_data);
static void request_app_quit(void);
static void on_pane_close_confirmed(gboolean confirmed, gpointer user_data);
static void on_workspace_close_confirmed(gboolean confirmed, gpointer user_data);
static void on_tab_close_confirmed(gboolean confirmed, gpointer user_data);
static gboolean focus_direction_for_layout_with_error(Workspace *ws,
                                                      int dx,
                                                      int dy,
                                                      const char *action,
                                                      const char **error_out);
static gboolean split_current_for_layout_action_with_error(
    Workspace *ws,
    GtkOrientation orientation,
    const char *action,
    const char **error_out);

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

static void
log_strip_unsupported_action(const char *action)
{
    if (!action)
        return;
    g_warning("Action '%s' is not supported in strip layout", action);
}

static gboolean
set_action_error(const char **error_out, const char *message)
{
    if (error_out)
        *error_out = message;
    return FALSE;
}

static gboolean
set_default_action_error(const char **error_out, const char *message)
{
    if (error_out && *error_out)
        return FALSE;
    return set_action_error(error_out, message);
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

gboolean
app_actions_is_quitting(void)
{
    return g_app_quit_in_progress;
}

static void
save_session_now(void)
{
    if (g_main_window) {
        session_save(g_main_window,
                     ui.terminal_stack, ui.workspace_list);
    }
}

static void
on_clipboard_text_received(GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
    ghostty_surface_t surface = (ghostty_surface_t)user_data;
    GError *error = NULL;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source),
                                                 result, &error);
    if (text && text[0] && surface)
        ghostty_surface_text(surface, text, strlen(text));
    g_free(text);
    if (error)
        g_error_free(error);
}

void
app_actions_open_url_in_preferred_target(const char *url)
{
    GError *error = NULL;

    if (!url || !url[0])
        return;

    if (!g_app_info_launch_default_for_uri(url, NULL, &error)) {
        g_warning("Failed to open URL in system browser: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

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
    GApplication *app;

    if (g_app_quit_in_progress)
        return;

    save_session_now();
    g_app_quit_in_progress = TRUE;
    session_begin_shutdown();
    workspace_set_shutting_down();
    socket_server_stop();

    app = g_application_get_default();
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

void
app_actions_request_app_quit_async(void)
{
    g_idle_add(quit_window_idle_cb, NULL);
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

    if (!confirmed || !pending || !pending->ws)
        return;

    if (workspace_get_layout_mode(pending->ws) == WORKSPACE_LAYOUT_CLASSIC) {
        if (pending->pane) {
            workspace_close_pane(pending->ws, pending->pane);
            session_queue_save();
        }
        return;
    }

    if (pending->pane)
        workspace_focus_pane(pending->ws, pending->pane);

    if (workspace_close_current_for_layout(pending->ws))
        session_queue_save();
}

static gboolean
focus_direction_for_layout(Workspace *ws, int dx, int dy, const char *action)
{
    return focus_direction_for_layout_with_error(ws, dx, dy, action, NULL);
}

static gboolean
focus_direction_for_layout_with_error(Workspace *ws,
                                      int dx,
                                      int dy,
                                      const char *action,
                                      const char **error_out)
{
    if (!ws)
        return set_action_error(error_out, "no active workspace");

    /* Keyboard/IPC pane navigation wins over hover-focus until the mouse
     * actually moves, so the keyboard-chosen pane isn't overridden by the
     * pane the pointer happens to sit over. */
    hover_focus_suppress_until_motion();

    if (workspace_get_layout_mode(ws) != WORKSPACE_LAYOUT_STRIP) {
        workspace_navigate_pane(ws, dx, dy);
        return TRUE;
    }

    if (dx < 0 && dy == 0)
        return workspace_focus_prev_for_layout(ws);
    if (dx > 0 && dy == 0)
        return workspace_focus_next_for_layout(ws);
    if (dy < 0 && dx == 0)
        return workspace_strip_focus_pane_up(ws);
    if (dy > 0 && dx == 0)
        return workspace_strip_focus_pane_down(ws);

    log_strip_unsupported_action(action);
    return set_action_error(error_out,
                            "action is unsupported in strip layout");
}

static gboolean
split_current_for_layout_action(Workspace *ws,
                                GtkOrientation orientation,
                                const char *action)
{
    return split_current_for_layout_action_with_error(ws, orientation,
                                                      action, NULL);
}

static gboolean
split_current_for_layout_action_with_error(Workspace *ws,
                                           GtkOrientation orientation,
                                           const char *action,
                                           const char **error_out)
{
    if (!ws)
        return set_action_error(error_out, "no active workspace");

    if (!workspace_split_current_for_layout(ws, orientation, g_ghostty_app)) {
        if (workspace_get_layout_mode(ws) == WORKSPACE_LAYOUT_STRIP)
            log_strip_unsupported_action(action);
        return set_action_error(error_out,
                                "action failed for current layout");
    }

    return TRUE;
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
        current_workspace >= (int)workspaces->len) {
        return;
    }

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
        (!ws->pane_notebooks || ws->pane_notebooks->len <= 1)) {
        return;
    }

    pending = g_new0(PendingMainTabClose, 1);
    pending->ws = ws;
    pending->notebook = g_object_ref(focused);
    pending->page = pg;
    close_confirm_request(g_main_window, CLOSE_CONFIRM_TAB,
                          on_tab_close_confirmed, pending,
                          pending_main_tab_close_free);
}

void
app_actions_handle(const char *action)
{
    if (g_str_has_prefix(action, "workspace.focus.")) {
        const char *suffix = action + strlen("workspace.focus.");
        int idx = atoi(suffix) - 1;
        if (workspaces && idx >= 0 && idx < (int)workspaces->len)
            workspace_switch(idx, ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "workspace.new") == 0) {
        workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
    } else if (strcmp(action, "workspace.close") == 0) {
        request_close_current_workspace();
    } else if (strcmp(action, "workspace.next") == 0 ||
               strcmp(action, "workspace.next.alt") == 0) {
        if (!workspaces || workspaces->len == 0)
            return;
        workspace_switch((current_workspace + 1) % workspaces->len,
                         ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "workspace.prev") == 0 ||
               strcmp(action, "workspace.prev.alt") == 0) {
        if (!workspaces || workspaces->len == 0)
            return;
        workspace_switch((current_workspace - 1 + workspaces->len) %
                             workspaces->len,
                         ui.terminal_stack, ui.workspace_list);
    } else if (strcmp(action, "pane.tab.new") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws) {
            GtkNotebook *focused = workspace_get_focused_pane(ws);
            if (focused && GTK_IS_NOTEBOOK(focused))
                workspace_add_terminal_to_focused(ws, g_ghostty_app);
            else
                workspace_add_terminal(ws, g_ghostty_app);
        }
    } else if (strcmp(action, "tab.next") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_tab_next_for_layout(ws);
    } else if (strcmp(action, "tab.prev") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_tab_prev_for_layout(ws);
    } else if (strcmp(action, "pane.focus.left") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            focus_direction_for_layout(ws, -1, 0, action);
    } else if (strcmp(action, "pane.focus.right") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            focus_direction_for_layout(ws, 1, 0, action);
    } else if (strcmp(action, "pane.focus.up") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            focus_direction_for_layout(ws, 0, -1, action);
    } else if (strcmp(action, "pane.focus.down") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            focus_direction_for_layout(ws, 0, 1, action);
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
        if (ui.overlay) {
            pane_move_overlay_toggle(GTK_OVERLAY(ui.overlay),
                                     ui.terminal_stack,
                                     ui.workspace_list);
        }
    } else if (strcmp(action, "pane.close") == 0) {
        request_close_current_pane(workspace_get_current());
    } else if (strcmp(action, "broadcast.toggle") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            ws->broadcast = !ws->broadcast;
    } else if (strcmp(action, "split.horizontal") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            split_current_for_layout_action(ws, GTK_ORIENTATION_HORIZONTAL, action);
    } else if (strcmp(action, "split.vertical") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            split_current_for_layout_action(ws, GTK_ORIENTATION_VERTICAL, action);
    } else if (strcmp(action, "window.fullscreen") == 0) {
        if (gtk_window_is_fullscreen(g_main_window))
            gtk_window_unfullscreen(g_main_window);
        else
            gtk_window_fullscreen(g_main_window);
    } else if (strcmp(action, "search.show") == 0) {
        if (ui.command_palette)
            command_palette_toggle(COMMAND_PALETTE(ui.command_palette));
    } else if (strcmp(action, "shortcuts.show") == 0) {
        if (ui.overlay)
            shortcuts_overlay_toggle(GTK_OVERLAY(ui.overlay));
    } else if (strcmp(action, "pane.zoom") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_layout_toggle_zoom_current(ws);
    } else if (strcmp(action, "notes.toggle") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws && ui.terminal_box)
            workspace_toggle_notes(ws, ui.terminal_box);
    } else if (strcmp(action, "terminal.search") == 0) {
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
    } else if (strcmp(action, "terminal.copy") == 0) {
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

gboolean
app_actions_handle_for_socket(const char *action,
                              gboolean non_interactive,
                              const char **error_out)
{
    Workspace *ws;

    if (error_out)
        *error_out = NULL;
    if (!action || !action[0])
        return set_action_error(error_out, "missing action name");

    if (strcmp(action, "pane.focus.left") == 0) {
        ws = workspace_get_current();
        if (!focus_direction_for_layout_with_error(ws, -1, 0, action, error_out))
            return set_default_action_error(error_out, "failed to focus pane");
        session_queue_save();
        return TRUE;
    }
    if (strcmp(action, "pane.focus.right") == 0) {
        ws = workspace_get_current();
        if (!focus_direction_for_layout_with_error(ws, 1, 0, action, error_out))
            return set_default_action_error(error_out, "failed to focus pane");
        session_queue_save();
        return TRUE;
    }
    if (strcmp(action, "pane.focus.up") == 0) {
        ws = workspace_get_current();
        if (!focus_direction_for_layout_with_error(ws, 0, -1, action, error_out))
            return set_default_action_error(error_out, "failed to focus pane");
        session_queue_save();
        return TRUE;
    }
    if (strcmp(action, "pane.focus.down") == 0) {
        ws = workspace_get_current();
        if (!focus_direction_for_layout_with_error(ws, 0, 1, action, error_out))
            return set_default_action_error(error_out, "failed to focus pane");
        session_queue_save();
        return TRUE;
    }

    if (strcmp(action, "split.horizontal") == 0) {
        ws = workspace_get_current();
        if (!split_current_for_layout_action_with_error(
                ws, GTK_ORIENTATION_HORIZONTAL, action, error_out)) {
            return set_default_action_error(error_out,
                                            "failed to split current pane");
        }
        session_queue_save();
        return TRUE;
    }
    if (strcmp(action, "split.vertical") == 0) {
        ws = workspace_get_current();
        if (!split_current_for_layout_action_with_error(
                ws, GTK_ORIENTATION_VERTICAL, action, error_out)) {
            return set_default_action_error(error_out,
                                            "failed to split current pane");
        }
        session_queue_save();
        return TRUE;
    }

    if (strcmp(action, "tab.next") == 0) {
        ws = workspace_get_current();
        if (!ws)
            return set_action_error(error_out, "no active workspace");
        if (!workspace_tab_next_for_layout(ws))
            return set_action_error(error_out, "no next tab");
        return TRUE;
    }
    if (strcmp(action, "tab.prev") == 0) {
        ws = workspace_get_current();
        if (!ws)
            return set_action_error(error_out, "no active workspace");
        if (!workspace_tab_prev_for_layout(ws))
            return set_action_error(error_out, "no previous tab");
        return TRUE;
    }

    if (strcmp(action, "pane.close") == 0) {
        ws = workspace_get_current();
        if (!ws)
            return set_action_error(error_out, "no active workspace");

        if (non_interactive &&
            workspace_get_layout_mode(ws) == WORKSPACE_LAYOUT_STRIP) {
            if (!workspace_close_current_for_layout(ws)) {
                return set_action_error(error_out,
                                        "failed to close active strip pane");
            }
            session_queue_save();
            return TRUE;
        }

        if (non_interactive) {
            return set_action_error(
                error_out,
                "pane.close requires interactive confirmation outside strip layout");
        }

        request_close_current_pane(ws);
        session_queue_save();
        return TRUE;
    }

    app_actions_handle(action);
    return TRUE;
}

gboolean
app_actions_on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                           guint keycode, GdkModifierType state,
                           gpointer user_data)
{
    (void)ctrl;
    (void)keycode;
    (void)user_data;

    GdkModifierType mods = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                    GDK_ALT_MASK | GDK_SUPER_MASK);
    guint lower = gdk_keyval_to_lower(keyval);

    if (g_terminal_search_target &&
        focus_within_terminal(g_terminal_search_target) &&
        terminal_search_handle_key(keyval, state)) {
        return TRUE;
    }

    if ((lower == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) &&
        (mods == GDK_CONTROL_MASK ||
         mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
        Workspace *ws = workspace_get_current();
        if (ws && (mods & GDK_SHIFT_MASK))
            workspace_tab_prev_for_layout(ws);
        else if (ws)
            workspace_tab_next_for_layout(ws);
        return TRUE;
    }

    {
        const char *action = shortcut_match(keyval, state);
        if (action) {
            const ShortcutDef *binding = shortcut_find_by_action(action);
            g_autofree char *keys = shortcut_format_binding(binding);

            if (strcmp(action, "recording.mark") != 0)
                shortcut_log_event("shortcut", action, keys);
            app_actions_handle(action);
            return TRUE;
        }
    }

    {
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
    }

    return FALSE;
}

gboolean
app_actions_on_close_request(GtkWindow *window, gpointer user_data)
{
    (void)window;
    (void)user_data;

    if (g_app_quit_in_progress)
        return FALSE;

    request_app_quit();
    return TRUE;
}

gboolean
app_actions_on_unix_quit_signal(gpointer user_data)
{
    (void)user_data;
    perform_app_quit();
    return G_SOURCE_CONTINUE;
}
