/*
 * pane_move_overlay.h - Overlay for moving the current terminal tab
 *
 * Shows a searchable list of destination panes across all workspaces.
 */
#pragma once

#include <gtk/gtk.h>

void pane_move_overlay_toggle(GtkOverlay *overlay,
                              GtkWidget *terminal_stack,
                              GtkWidget *workspace_list);
