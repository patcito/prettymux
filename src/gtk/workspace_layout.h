#pragma once

#include <gtk/gtk.h>

typedef enum {
    WORKSPACE_LAYOUT_CLASSIC = 0,
    WORKSPACE_LAYOUT_STRIP   = 1,
} WorkspaceLayoutMode;

typedef struct _Workspace Workspace;

WorkspaceLayoutMode workspace_get_layout_mode(Workspace *ws);
void                workspace_set_layout_mode(Workspace *ws,
                                              WorkspaceLayoutMode mode);

GtkWidget *workspace_layout_create_root(Workspace *ws, GtkWidget *first_notebook);
void       workspace_layout_focus_primary(Workspace *ws);
void       workspace_layout_toggle_zoom_current(Workspace *ws);
gboolean   workspace_layout_is_zoomed(Workspace *ws);
gboolean   workspace_rebuild_for_layout_mode(Workspace *ws,
                                             WorkspaceLayoutMode mode);
