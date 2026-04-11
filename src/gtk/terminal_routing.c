#include "terminal_routing.h"

#include <string.h>

#include "app_state.h"
#include "app_support.h"
#include "notifications.h"
#include "port_scanner.h"
#include "workspace.h"

SurfaceLookup
terminal_routing_find_for_surface(ghostty_surface_t surface)
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

                if (ws->pane_notebooks) {
                    for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
                        GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
                        int n_pages = gtk_notebook_get_n_pages(nb);
                        for (int pg = 0; pg < n_pages; pg++) {
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

SurfaceLookup
terminal_routing_find_for_id(const char *terminal_id)
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

void
terminal_routing_on_port_scanner_detected(const char *terminal_id,
                                          int         port,
                                          gpointer    user_data)
{
    (void)user_data;
    terminal_routing_handle_reported_port(terminal_id, port);
}

void
terminal_routing_register_scope(const char *terminal_id,
                                pid_t       session_id,
                                const char *tty_name,
                                const char *tty_path)
{
    SurfaceLookup loc;

    if (!terminal_id || !terminal_id[0])
        return;

    loc = terminal_routing_find_for_id(terminal_id);
    if (!loc.terminal)
        return;

    ghostty_terminal_set_scope(loc.terminal, session_id, tty_name, tty_path);
    port_scanner_register_terminal(terminal_id, session_id, tty_name,
                                   tty_path, loc.workspace_idx);
}

void
terminal_routing_handle_reported_port(const char *terminal_id, int port)
{
    SurfaceLookup loc;
    Workspace *ws;
    char msg[64];
    char notif_msg[96];

    if (!terminal_id || !terminal_id[0] || port <= 0)
        return;

    loc = terminal_routing_find_for_id(terminal_id);
    ws = loc.workspace;

    if (loc.workspace_idx >= 0)
        port_scanner_set_terminal_workspace(terminal_id, loc.workspace_idx);

    if (!workspace_should_show_port_notification(ws))
        return;

    snprintf(ws->notification, sizeof(ws->notification), "-> localhost:%d", port);
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
