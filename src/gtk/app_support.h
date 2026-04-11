#pragma once

#include <gtk/gtk.h>

void shortcut_log_event(const char *type, const char *action, const char *keys);

void app_support_set_main_window_active(gboolean active);
const char *prettymux_icon_name(void);
void ensure_local_desktop_entry(void);
void debug_notification_log(const char *fmt, ...);
void send_desktop_notification(const char *title, const char *body,
                               int ws_idx, int pane_idx, int tab_idx);
void about_dialog_present(GtkWindow *parent);
void show_welcome_dialog(GtkWindow *parent);
