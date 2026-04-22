#include "app_actions.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "app_settings.h"
#include "app_support.h"
#include "app_state.h"
#include "browser_tab.h"
#include "close_confirm.h"
#include "command_palette.h"
#include "ghostty.h"
#include "ghostty_terminal.h"
#include "notifications.h"
#include "pane_move_overlay.h"
#include "pip_window.h"
#include "session.h"
#include "settings_dialog.h"
#include "shortcuts.h"
#include "shortcuts_overlay.h"
#include "socket_server.h"
#include "theme.h"
#include "workspace.h"

static const char *k_default_browser_url =
    "https://prettymux-web.vercel.app/?prettymux=t";

static gboolean g_app_quit_in_progress = FALSE;

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

static void save_session_now(void);
static gboolean quit_window_idle_cb(gpointer data);
static void request_close_current_browser_tab(void);
static void request_close_current_tab(Workspace *ws);
static void request_close_current_pane(Workspace *ws);
static void request_close_current_workspace(void);
static void on_clipboard_text_received(GObject *source,
                                       GAsyncResult *result,
                                       gpointer user_data);
static void on_browser_title_changed(BrowserTab *bt,
                                     const char *title,
                                     gpointer lbl);
static void on_browser_new_tab_requested(BrowserTab *bt,
                                         const char *url,
                                         gpointer user_data);
static void on_new_browser_tab_clicked(GtkButton *button, gpointer user_data);
static void perform_app_quit(void);
static void on_app_close_confirmed(gboolean confirmed, gpointer user_data);
static void request_app_quit(void);
static void on_pane_close_confirmed(gboolean confirmed, gpointer user_data);
static void on_workspace_close_confirmed(gboolean confirmed, gpointer user_data);
static void on_tab_close_confirmed(gboolean confirmed, gpointer user_data);
static void on_browser_tab_close_confirmed(gboolean confirmed,
                                           gpointer user_data);

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
        session_save(g_main_window, ui.browser_notebook,
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

static void
on_browser_title_changed(BrowserTab *bt, const char *title, gpointer lbl)
{
    (void)bt;
    const char *safe_title = title ? title : "";
    char s[24];

    snprintf(s, sizeof(s), "%.20s", safe_title);
    gtk_label_set_text(GTK_LABEL(lbl), s);
}

static void
on_browser_new_tab_requested(BrowserTab *bt, const char *url, gpointer user_data)
{
    (void)bt;
    (void)user_data;
    app_actions_add_browser_tab(url);
    session_queue_save();
}

static void
on_browser_close_btn_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    request_close_current_browser_tab();
}

void
app_actions_add_browser_tab(const char *url)
{
    GtkWidget *tab = browser_tab_new(url);
    GtkWidget *label = gtk_label_new("Loading...");
    GtkWidget *tab_label = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(tab_label), label);

    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close_btn), FALSE);
    gtk_widget_set_focusable(close_btn, FALSE);
    gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
    g_signal_connect(close_btn, "clicked",
                     G_CALLBACK(on_browser_close_btn_clicked), NULL);
    gtk_box_append(GTK_BOX(tab_label), close_btn);

    int idx;

    g_signal_connect_object(tab, "title-changed",
                            G_CALLBACK(on_browser_title_changed), label, 0);
    g_signal_connect(tab, "new-tab-requested",
                     G_CALLBACK(on_browser_new_tab_requested), NULL);

    idx = gtk_notebook_append_page(GTK_NOTEBOOK(ui.browser_notebook), tab, tab_label);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(ui.browser_notebook), tab, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(ui.browser_notebook), idx);
    gtk_widget_set_visible(tab, TRUE);
}

void
app_actions_open_url_in_preferred_target(const char *url)
{
    GError *error = NULL;

    if (!url || !url[0])
        return;

    if (app_settings_get_open_links_in_browser()) {
        app_actions_add_browser_tab(url);
        gtk_widget_set_visible(ui.browser_notebook, TRUE);
        return;
    }

    if (!g_app_info_launch_default_for_uri(url, NULL, &error)) {
        g_warning("Failed to open URL in system browser: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
    }
}

static void
on_new_browser_tab_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    app_actions_add_browser_tab(k_default_browser_url);
    session_queue_save();
}

void
app_actions_build_browser(void)
{
    GtkWidget *btn;

    ui.browser_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(ui.browser_notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(ui.browser_notebook), FALSE);

    btn = gtk_button_new_with_label("+");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_new_browser_tab_clicked), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(ui.browser_notebook), btn,
                                   GTK_PACK_END);
    gtk_widget_set_visible(btn, TRUE);
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
    } else if (strcmp(action, "pane.focus.left") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_navigate_pane(ws, -1, 0);
    } else if (strcmp(action, "pane.focus.right") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_navigate_pane(ws, 1, 0);
    } else if (strcmp(action, "pane.focus.up") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_navigate_pane(ws, 0, -1);
    } else if (strcmp(action, "pane.focus.down") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_navigate_pane(ws, 0, 1);
    } else if (strcmp(action, "browser.toggle") == 0) {
        gboolean vis = gtk_widget_get_visible(ui.browser_notebook);
        gtk_widget_set_visible(ui.browser_notebook, !vis);
    } else if (strcmp(action, "browser.new") == 0 ||
               strcmp(action, "browser.tab.new") == 0) {
        app_actions_add_browser_tab(k_default_browser_url);
        gtk_widget_set_visible(ui.browser_notebook, TRUE);
    } else if (strcmp(action, "browser.tab.close") == 0) {
        request_close_current_browser_tab();
    } else if (strcmp(action, "devtools.docked") == 0 ||
               strcmp(action, "devtools.window") == 0) {
        int pg = gtk_notebook_get_current_page(GTK_NOTEBOOK(ui.browser_notebook));
        if (pg >= 0) {
            GtkWidget *child =
                gtk_notebook_get_nth_page(GTK_NOTEBOOK(ui.browser_notebook), pg);
            if (BROWSER_IS_TAB(child))
                browser_tab_show_inspector(BROWSER_TAB(child));
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
            workspace_split_pane(ws, GTK_ORIENTATION_HORIZONTAL, g_ghostty_app);
    } else if (strcmp(action, "split.vertical") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_split_pane(ws, GTK_ORIENTATION_VERTICAL, g_ghostty_app);
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
    } else if (strcmp(action, "pip.toggle") == 0) {
        pip_window_toggle(g_main_window, ui.browser_notebook);
    } else if (strcmp(action, "pane.zoom") == 0) {
        Workspace *ws = workspace_get_current();
        if (ws)
            workspace_toggle_zoom(ws);
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
    } else if (strcmp(action, "browser.focus_url") == 0) {
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
        if (ws) {
            GtkNotebook *nb = workspace_get_focused_pane(ws);
            if (nb) {
                int n = gtk_notebook_get_n_pages(nb);
                if (n > 1) {
                    int cur = gtk_notebook_get_current_page(nb);
                    int next = (mods & GDK_SHIFT_MASK)
                        ? (cur - 1 + n) % n
                        : (cur + 1) % n;
                    gtk_notebook_set_current_page(nb, next);
                }
            }
        }
        return TRUE;
    }

    {
        const char *action = shortcut_match(keyval, state);
        if (action) {
            const ShortcutDef *binding = shortcut_find_by_action(action);
            g_autofree char *keys = shortcut_format_binding(binding);

            if (strcmp(action, "browser.tab.close") == 0) {
                GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(g_main_window));
                if (!focus || !gtk_widget_is_ancestor(focus, ui.browser_notebook))
                    return FALSE;
            }
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
