#pragma once

#include <gtk/gtk.h>

typedef struct _Workspace Workspace;

void       sidebar_ui_build_notification_preview_section(GtkWidget *section_label,
                                                         const char *preview,
                                                         gboolean enabled);
void       sidebar_ui_build_branch_cwd_section(GtkWidget *section_label,
                                               const char *cwd,
                                               const char *branch,
                                               gboolean enabled);
void       sidebar_ui_build_workspace_status_section(GtkWidget *section_box,
                                                     GPtrArray *status_entries,
                                                     int max_entries);
void       sidebar_ui_build_progress_section(GtkWidget *section_label,
                                             int progress_state,
                                             int progress_percent,
                                             gboolean enabled);
void       sidebar_ui_build_structure_indicator_section(GtkWidget *section_label,
                                                        gboolean strip_mode,
                                                        int pane_or_column_count,
                                                        int tab_count,
                                                        gboolean enabled);
