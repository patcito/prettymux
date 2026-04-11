#pragma once

#include <gtk/gtk.h>

void app_actions_add_browser_tab(const char *url);
void app_actions_open_url_in_preferred_target(const char *url);
void app_actions_build_browser(void);
void app_actions_handle(const char *action);
gboolean app_actions_on_key_pressed(GtkEventControllerKey *ctrl,
                                    guint                  keyval,
                                    guint                  keycode,
                                    GdkModifierType        state,
                                    gpointer               user_data);
gboolean app_actions_on_close_request(GtkWindow *window, gpointer user_data);
gboolean app_actions_on_unix_quit_signal(gpointer user_data);
void app_actions_request_app_quit_async(void);
gboolean app_actions_is_quitting(void);
