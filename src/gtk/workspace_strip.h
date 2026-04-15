#pragma once

#include <gtk/gtk.h>

typedef struct _Workspace Workspace;

typedef struct {
    GtkWidget *notebook;      /* Root widget in column_box (notebook or GtkPaned chain) */
    GPtrArray *panes;         /* All GtkNotebook* in this column, top-to-bottom */
    int        focused_pane;  /* Index within panes of the focused pane */
    int        target_width;
    double     current_width;
    gboolean   maximized;
} WorkspaceColumn;

typedef enum {
    WORKSPACE_STRIP_ANIM_DEFAULT = 0,
    WORKSPACE_STRIP_ANIM_INSERT,
    WORKSPACE_STRIP_ANIM_REMOVE,
    WORKSPACE_STRIP_ANIM_MAXIMIZE,
} WorkspaceStripAnimProfile;

typedef struct _WorkspaceStripState {
    GPtrArray *columns;          /* Array of WorkspaceColumn* */
    int        focused_col;      /* Index of the focused column */
    double     camera_x;         /* Horizontal scroll offset (pixels) */
    double     camera_target_x;  /* Animation target for camera_x */
    GtkWidget *scroll_container; /* The scrolling viewport */
    GtkWidget *column_box;       /* Horizontal box holding columns */
    guint      tick_id;          /* Active tick callback id, or 0 */
    gint64     last_tick_usec;   /* Last frame time for dt-based easing */
    WorkspaceStripAnimProfile anim_profile; /* Current animation tuning */
} WorkspaceStripState;

WorkspaceStripState *workspace_strip_state_new(void);
void                 workspace_strip_state_free(WorkspaceStripState *state);

void       workspace_strip_init(Workspace *ws);
GtkWidget *workspace_strip_create_root(Workspace *ws, GtkWidget *first_notebook);
void       workspace_strip_apply_layout(Workspace *ws);
void       workspace_strip_focus_column(Workspace *ws, int col_idx);
void       workspace_strip_pan_to_focused_column(Workspace *ws);
void       workspace_strip_clamp_camera(WorkspaceStripState *state,
                                        int viewport_width);
gboolean   workspace_strip_tick_cb(GtkWidget *widget,
                                   GdkFrameClock *frame_clock,
                                   gpointer user_data);
void       workspace_strip_focus_primary(Workspace *ws);
gboolean   workspace_strip_insert_column_after_active(Workspace *ws,
                                                      GtkWidget *notebook);
gboolean   workspace_strip_remove_active_column(Workspace *ws);
void       workspace_strip_toggle_maximize_column(Workspace *ws);
void       workspace_strip_toggle_zoom(Workspace *ws);
void       workspace_strip_add_notebook_column(Workspace *ws,
                                               GtkWidget *notebook);
gboolean   workspace_strip_split_vertical_in_column(Workspace *ws,
                                                     GtkWidget *new_notebook);
gboolean   workspace_strip_focus_pane_up(Workspace *ws);
gboolean   workspace_strip_focus_pane_down(Workspace *ws);
GtkNotebook *workspace_strip_column_focused_notebook(WorkspaceColumn *col);
gboolean   workspace_strip_remove_pane_from_column(Workspace *ws,
                                                    GtkNotebook *pane);
int        workspace_strip_column_pane_count(Workspace *ws, int col_idx);
