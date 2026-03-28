#pragma once

#include <gtk/gtk.h>
#include "ghostty_terminal.h"

// Show a history overlay with searchable bash history.
// On selection, the command is typed into the given terminal.
GtkWidget *history_overlay_new(void);
void history_overlay_show(GtkWidget *overlay, GhosttyTerminal *target_terminal);
void history_overlay_hide(GtkWidget *overlay);
