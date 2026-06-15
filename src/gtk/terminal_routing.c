#include "terminal_routing.h"

#include <string.h>

#include "app_state.h"
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

    /* session_id is still needed: shutdown sends SIGHUP to the process group
     * via ghostty_terminal_hangup_session(). tty_name/tty_path are no longer
     * used now that the port scanner is gone, but harmless to record. */
    ghostty_terminal_set_scope(loc.terminal, session_id, tty_name, tty_path);
}
