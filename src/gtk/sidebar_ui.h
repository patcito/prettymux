#pragma once

#include <gtk/gtk.h>

typedef struct _Workspace Workspace;

void       sidebar_ui_build(void);
void       sidebar_ui_show_move_to_window_menu(Workspace *workspace);

/* Cancel a pending deferred workspace-row activation. Called when a
 * double-click rename gesture fires so the terminal focus-grab from
 * the first click doesn't steal focus from the rename entry. */
void       sidebar_ui_cancel_pending_activate(void);
GtkWidget *sidebar_ui_build_workspace_card(GtkWidget  *header_box,
                                           GtkWidget **out_meta_label,
                                           GtkWidget **out_status_label,
                                           GtkWidget **out_status_entries_box,
                                           GtkWidget **out_ports_label,
                                           GtkWidget **out_progress_label,
                                           GtkWidget **out_structure_label,
                                           GtkWidget **out_badge);
