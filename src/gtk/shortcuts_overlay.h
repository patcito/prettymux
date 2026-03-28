/*
 * shortcuts_overlay.h - Keyboard shortcuts overlay
 *
 * Shows all keyboard shortcuts in a two-column list overlay.
 * Toggle with Ctrl+Shift+K or close with Escape.
 */
#pragma once

#include <gtk/gtk.h>

/*
 * shortcuts_overlay_toggle:
 * @overlay: the GtkOverlay on which to show the shortcuts card
 *
 * If the overlay is currently showing, removes and destroys it.
 * If not showing, creates and displays the shortcuts overlay.
 */
void shortcuts_overlay_toggle(GtkOverlay *overlay);
