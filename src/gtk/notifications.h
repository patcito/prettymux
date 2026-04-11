#pragma once

#include <gtk/gtk.h>

void notifications_init(void);
void notifications_add_full(const char *msg, int ws_idx,
                            GtkNotebook *pane, int tab_idx);
void notifications_clear(void);
guint notifications_count(void);
void bell_button_update(void);
void notifications_apply_toast_position_setting(void);
gboolean notification_target_is_active(int ws_idx,
                                       GtkNotebook *pane_notebook,
                                       int tab_idx);
void navigate_to_notification_target(int ws_idx,
                                     GtkNotebook *pane_notebook,
                                     int tab_idx);
void sidebar_toast_hide(void);
void sidebar_toast_show(const char *msg, int ws_idx,
                        GtkNotebook *pane_notebook, int tab_idx);
void sidebar_toast_show_copy(const char *msg, int ws_idx,
                             GtkNotebook *pane_notebook, int tab_idx);
void sidebar_toast_show_internal(const char *msg, int ws_idx,
                                 GtkNotebook *pane_notebook, int tab_idx,
                                 gboolean force_bottom,
                                 gboolean copy_style,
                                 gboolean recording_marker,
                                 guint timeout_ms);
void notifications_on_bell_button_clicked(GtkButton *btn, gpointer user_data);
void build_toast_overlay(void);
