#pragma once

#include <gtk/gtk.h>
#include "ghostty.h"

/* Callback for adding a browser tab during session restore.
 * Allows the caller (main.c) to wire up signal handlers. */
typedef void (*SessionAddBrowserTabFunc)(const char *url);

void session_save(GtkWindow *window, GtkWidget *browser_notebook,
                  GtkWidget *terminal_stack, GtkWidget *workspace_list);
void session_restore(GtkWindow *window, GtkWidget *browser_notebook,
                     GtkWidget *terminal_stack, GtkWidget *workspace_list,
                     ghostty_app_t ghostty_app,
                     SessionAddBrowserTabFunc add_browser_tab_func);
gboolean session_exists(void);
