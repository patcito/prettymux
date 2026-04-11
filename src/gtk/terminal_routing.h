#pragma once

#include <gtk/gtk.h>

#include "ghostty.h"
#include "ghostty_terminal.h"
#include "workspace.h"

typedef struct {
    GhosttyTerminal *terminal;
    Workspace       *workspace;
    int              workspace_idx;
    GtkNotebook     *pane_notebook;
    int              pane_idx;
    int              tab_idx;
} SurfaceLookup;

SurfaceLookup terminal_routing_find_for_surface(ghostty_surface_t surface);
SurfaceLookup terminal_routing_find_for_id(const char *terminal_id);
void terminal_routing_register_scope(const char *terminal_id,
                                     pid_t       session_id,
                                     const char *tty_name,
                                     const char *tty_path);
void terminal_routing_handle_reported_port(const char *terminal_id, int port);
void terminal_routing_on_port_scanner_detected(const char *terminal_id,
                                               int         port,
                                               gpointer    user_data);
