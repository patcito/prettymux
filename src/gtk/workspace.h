#pragma once

#include <gtk/gtk.h>
#include "ghostty.h"

typedef struct {
    char name[64];
    GtkWidget *container;        /* Root widget for this workspace in the stack.
                                  * Always the workspace overlay. */
    GtkWidget *overlay;          /* Root overlay: pane tree child + floating terminals */
    GtkWidget *notebook;         /* The *first* terminal tab notebook (kept for
                                  * backwards compat; same as pane_notebooks[0]). */
    GPtrArray *terminals;        /* Flat array of ALL GhosttyTerminal widgets */
    GPtrArray *pane_notebooks;   /* Array of GtkNotebook* -- one per pane */
    GtkNotebook *active_pane;    /* Last pane activated by hover/click/focus */
    char cwd[512];
    char git_branch[128];
    char notification[256];
    gboolean broadcast;
    GtkWidget *sidebar_label;    /* GtkLabel in the sidebar row (for updates) */

    /* Pane zoom state */
    gboolean zoomed;
    GtkNotebook *zoomed_pane;

    /* Notes panel */
    char *notes_text;            /* Per-workspace notes content (heap-allocated) */
} Workspace;

extern GPtrArray *workspaces;
extern int current_workspace;

/* Keep references to terminal_stack / workspace_list for DnD operations */
extern GtkWidget *g_terminal_stack;
extern GtkWidget *g_workspace_list;

Workspace *workspace_get_current(void);
void workspace_add(GtkWidget *terminal_stack, GtkWidget *workspace_list, ghostty_app_t app);
void workspace_remove(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list);
void workspace_switch(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list);
void workspace_add_terminal(Workspace *ws, ghostty_app_t app);
void workspace_add_terminal_to_focused(Workspace *ws, ghostty_app_t app);
void workspace_add_terminal_to_notebook_external(Workspace *ws,
                                                  GtkNotebook *notebook,
                                                  ghostty_app_t app);
void workspace_add_terminal_to_notebook_with_cwd(Workspace *ws,
                                                  GtkNotebook *notebook,
                                                  ghostty_app_t app,
                                                  const char *cwd);

/*
 * Git branch detection (async).
 *
 * workspace_detect_git: spawn `git rev-parse --abbrev-ref HEAD` in the
 *   given workspace CWD and update ws->git_branch + sidebar label on
 *   completion.
 */
void workspace_detect_git(Workspace *ws);

/*
 * Sidebar label refresh.
 *
 * workspace_refresh_sidebar_label: update the sidebar row label to
 *   show "name [branch]" or just "name".
 */
void workspace_refresh_sidebar_label(Workspace *ws);

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

/*
 * Pane zoom.
 *
 * workspace_toggle_zoom: when zooming, hide all pane notebooks except
 *   the focused one.  When un-zooming, show all panes again.
 */
void workspace_toggle_zoom(Workspace *ws);

/*
 * Notes panel.
 *
 * workspace_toggle_notes: show/hide a per-workspace text editing area.
 *   On hide, saves the text to ws->notes_text.
 *   On show, restores from ws->notes_text.
 *
 * workspace_save_notes: save current notes text from the visible panel
 *   (call before switching workspaces).
 *
 * workspace_restore_notes: restore notes text to the panel for the
 *   given workspace (call after switching workspaces).
 */
void workspace_toggle_notes(Workspace *ws, GtkWidget *container);
void workspace_save_notes(Workspace *ws);
void workspace_restore_notes(Workspace *ws);

/*
 * Pane navigation.
 *
 * workspace_navigate_pane: move focus to the pane that is
 *   geometrically in the direction (dx, dy) from the currently
 *   focused pane.  dx/dy should be -1, 0, or 1.
 */
void workspace_navigate_pane(Workspace *ws, int dx, int dy);

/*
 * Tab label refresh.
 *
 * workspace_refresh_tab_labels: update all tab labels in a workspace to
 *   reflect activity indicators and progress bars.
 */
void workspace_refresh_tab_labels(Workspace *ws);

/*
 * Check if any terminal in the workspace has unread activity.
 */
gboolean workspace_has_activity(Workspace *ws);
void workspace_set_shutting_down(void);
gboolean workspace_move_tab(int src_ws_idx, int src_pane_idx, int src_tab_idx,
                            int dest_ws_idx, int dest_pane_idx);
gboolean workspace_select_tab(int ws_idx, int pane_idx, int tab_idx);
gboolean workspace_close_terminal(Workspace *ws, GtkWidget *terminal);
gboolean workspace_close_current_tab(Workspace *ws);

/* Trigger inline rename on the current tab's label */
void workspace_start_tab_rename(Workspace *ws);
