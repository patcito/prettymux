#pragma once

#include <gtk/gtk.h>
#include "ghostty.h"
#include "workspace_layout.h"

typedef struct _WorkspaceStripState WorkspaceStripState;

typedef struct workspace_status_entry {
    char entry_id[96];
    char provider[48];
    char kind[48];
    char status[64];
    char summary[192];
    char detail[320];
    gint64 updated_at_usec;
    gboolean attention;
} workspace_status_entry;

typedef struct _Workspace {
    WorkspaceLayoutMode layout_mode;
    WorkspaceStripState *strip_state;
    guint64 serial;              /* Stable runtime id for async callbacks */
    char name[64];
    GtkWidget *container;        /* Root widget for this workspace in the stack.
                                  * Always the workspace overlay. */
    gboolean detached_container_ref; /* True when detach holds a ref for re-attach */
    GtkWidget *overlay;          /* Root overlay: pane tree child + floating terminals */
    GtkWidget *notebook;         /* The *first* terminal tab notebook (kept for
                                  * backwards compat; same as pane_notebooks[0]). */
    GPtrArray *terminals;        /* Flat array of ALL GhosttyTerminal widgets */
    GPtrArray *pane_notebooks;   /* Array of GtkNotebook* -- one per pane */
    GtkNotebook *active_pane;    /* Last pane activated by hover/click/focus */
    char cwd[512];
    char git_branch[128];
    GCancellable *git_branch_cancel;
    guint64 git_branch_generation;
    char sidebar_primary_branch[128];
    GCancellable *primary_branch_cancel;
    guint64 primary_branch_generation;
    char notification[256];
    gboolean broadcast;
    GtkWidget *sidebar_label;    /* GtkLabel in the sidebar row (for updates) */
    GtkWidget *sidebar_meta_label;   /* Branch + cwd summary line */
    GtkWidget *sidebar_status_label; /* Recent notification preview line */
    GtkWidget *sidebar_status_entries_box; /* Recent structured status lines */
    GtkWidget *sidebar_ports_label;  /* Compact ports summary line */
    GtkWidget *sidebar_progress_label; /* Compact progress visualization */
    GtkWidget *sidebar_structure_label; /* Pane/column count indicator */
    GtkWidget *sidebar_badge;        /* Attention badge dot */
    GHashTable *status_entries;      /* entry_id => workspace_status_entry* */

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
int workspace_get_index(Workspace *ws);
int workspace_apply_layout_mode_to_all(WorkspaceLayoutMode mode);
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
GtkNotebook *workspace_split_pane_target(Workspace *ws, GtkNotebook *pane,
                                         GtkOrientation orientation,
                                         ghostty_app_t app);
gboolean    workspace_split_current_for_layout(Workspace *ws,
                                               GtkOrientation orientation,
                                               ghostty_app_t app);
void        workspace_close_pane(Workspace *ws, GtkNotebook *pane);
gboolean    workspace_close_current_for_layout(Workspace *ws);
GtkNotebook *workspace_get_focused_pane(Workspace *ws);
GtkNotebook *workspace_get_pane_by_index(Workspace *ws, int pane_idx);
GtkNotebook *workspace_get_pane_by_id(Workspace *ws, const char *pane_id);
int         workspace_get_pane_index(Workspace *ws, GtkNotebook *pane);
const char *workspace_get_pane_id(GtkNotebook *pane);
gboolean    workspace_focus_pane(Workspace *ws, GtkNotebook *pane);
gboolean    workspace_focus_next_for_layout(Workspace *ws);
gboolean    workspace_focus_prev_for_layout(Workspace *ws);
gboolean    workspace_tab_next_for_layout(Workspace *ws);
gboolean    workspace_tab_prev_for_layout(Workspace *ws);
gboolean    workspace_equalize_splits(Workspace *ws,
                                      const char *orientation_name);
gboolean    workspace_resize_pane_percent(Workspace *ws,
                                          GtkNotebook *pane,
                                          char axis,
                                          double percent);

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
void workspace_mark_tab_notification(GtkNotebook *pane, int page_num);
void workspace_clear_tab_notification(GtkNotebook *pane, int page_num);

/*
 * Check if any terminal in the workspace has unread activity.
 */
gboolean workspace_has_activity(Workspace *ws);

/*
 * Sidebar card data helpers.
 *
 * Primary-path rule: CWD and branch come from the first tab of the
 * first pane.  Falls back to ws->cwd / ws->git_branch only when the
 * first-pane terminal is unavailable.
 */
const char *workspace_get_sidebar_primary_cwd(Workspace *ws);
const char *workspace_get_sidebar_primary_branch(Workspace *ws);
char       *workspace_get_sidebar_status_summary(Workspace *ws);
void        workspace_set_status_entry(Workspace *ws,
                                       const workspace_status_entry *entry);
void        workspace_clear_status_entry(Workspace *ws,
                                         const char *entry_id);
GPtrArray  *workspace_get_sorted_status_entries(Workspace *ws);
int         workspace_get_sidebar_column_count(Workspace *ws);
int         workspace_get_sidebar_tab_count(Workspace *ws);
gboolean    workspace_get_sidebar_attention_state(Workspace *ws);
char       *workspace_get_sidebar_ports_summary(Workspace *ws);
gboolean    workspace_get_sidebar_progress(Workspace *ws,
                                           int *state_out,
                                           int *percent_out);

void workspace_focus_first_terminal(Workspace *ws);
void workspace_set_primary_notebook(Workspace *ws, GtkWidget *notebook);

void workspace_set_shutting_down(void);
void workspace_shutdown_terminals(void);
gboolean workspace_move_tab(int src_ws_idx, int src_pane_idx, int src_tab_idx,
                            int dest_ws_idx, int dest_pane_idx);
Workspace *workspace_detach_from_instance(int index);
gboolean workspace_attach_to_instance(Workspace *ws, int target_index);
gboolean workspace_import_from_payload(const char *payload,
                                       ghostty_app_t app,
                                       int *out_workspace_index,
                                       char **error_out);
gboolean workspace_move_to_instance(int source_workspace_index,
                                    const char *target_instance_id,
                                    int *out_target_workspace_index,
                                    char **error_out);
gboolean workspace_select_tab(int ws_idx, int pane_idx, int tab_idx);
gboolean workspace_close_tab_at(Workspace *ws, GtkNotebook *notebook, int page);
gboolean workspace_close_terminal(Workspace *ws, GtkWidget *terminal);
gboolean workspace_close_current_tab(Workspace *ws);

/* Trigger inline rename on the current tab's label */
void workspace_start_tab_rename(Workspace *ws);

/*
 * If the widget lives inside a workspace that uses strip layout, apply
 * dx_pixels of horizontal scroll to the strip camera and return TRUE.
 * Returns FALSE when the widget is outside any strip-mode workspace
 * (e.g. classic split layout) so the caller can handle the scroll itself.
 */
gboolean workspace_strip_consume_horizontal_scroll(GtkWidget *anchor,
                                                   double dx_pixels);

/*
 * Workspace drag-reorder.
 *
 * workspace_reorder: move a workspace from src_idx to dest_idx in the
 *   sidebar and the internal workspaces array. After reordering, the
 *   active workspace and sidebar selection are updated.
 */
void workspace_reorder(int src_idx, int dest_idx);
