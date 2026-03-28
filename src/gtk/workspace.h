#pragma once

#include <gtk/gtk.h>
#include "ghostty.h"

typedef struct {
    char name[64];
    GtkWidget *container;        /* Root widget for this workspace in the stack.
                                  * Either the notebook (no splits) or a GtkPaned tree. */
    GtkWidget *notebook;         /* The *first* terminal tab notebook (kept for
                                  * backwards compat; same as pane_notebooks[0]). */
    GPtrArray *terminals;        /* Flat array of ALL GhosttyTerminal widgets */
    GPtrArray *pane_notebooks;   /* Array of GtkNotebook* — one per pane */
    char cwd[512];
    char git_branch[128];
    char notification[256];
    gboolean broadcast;
} Workspace;

extern GPtrArray *workspaces;
extern int current_workspace;

Workspace *workspace_get_current(void);
void workspace_add(GtkWidget *terminal_stack, GtkWidget *workspace_list, ghostty_app_t app);
void workspace_remove(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list);
void workspace_switch(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list);
void workspace_add_terminal(Workspace *ws, ghostty_app_t app);
void workspace_add_terminal_to_focused(Workspace *ws, ghostty_app_t app);

/*
 * Pane splitting.
 *
 * workspace_split_pane: split the currently focused pane's notebook,
 *   wrapping it in a GtkPaned with a new sibling notebook.
 *
 * workspace_close_pane: remove a pane notebook and collapse its parent
 *   GtkPaned.  If it is the last pane, this is a no-op.
 *
 * workspace_get_focused_pane: return the GtkNotebook that contains the
 *   currently focused terminal, or the first notebook if none is focused.
 */
void        workspace_split_pane(Workspace *ws, GtkOrientation orientation,
                                 ghostty_app_t app);
void        workspace_close_pane(Workspace *ws, GtkNotebook *pane);
GtkNotebook *workspace_get_focused_pane(Workspace *ws);
