#pragma once

#include <gtk/gtk.h>
#include "ghostty.h"

void session_save(GtkWindow *window, GtkWidget *browser_notebook,
                  GtkWidget *terminal_stack, GtkWidget *workspace_list);
void session_restore(GtkWindow *window, GtkWidget *browser_notebook,
                     GtkWidget *terminal_stack, GtkWidget *workspace_list,
                     ghostty_app_t ghostty_app);
gboolean session_exists(void);
