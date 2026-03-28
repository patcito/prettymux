/*
 * command_palette.h - Search overlay / command palette widget
 *
 * A GtkWidget overlay that shows a search entry + filtered list of
 * workspaces, terminal tabs, and browser tabs.  Arrow keys navigate,
 * Enter activates, Escape closes.
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define COMMAND_TYPE_PALETTE (command_palette_get_type())
G_DECLARE_FINAL_TYPE(CommandPalette, command_palette, COMMAND, PALETTE, GtkWidget)

/*
 * command_palette_new:
 * @browser_notebook: the browser GtkNotebook (to enumerate browser tabs)
 * @terminal_stack:   the GtkStack containing workspace containers
 * @workspace_list:   the sidebar GtkListBox for workspace switching
 *
 * Creates a new command palette overlay.  The caller should add it to
 * a GtkOverlay on the main window.
 */
GtkWidget *command_palette_new(GtkWidget *browser_notebook,
                               GtkWidget *terminal_stack,
                               GtkWidget *workspace_list);

/*
 * command_palette_toggle:
 *
 * If the palette is currently visible, hides and destroys it.
 * If hidden, populates and shows it.
 */
void command_palette_toggle(CommandPalette *self);

G_END_DECLS
