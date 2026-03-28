/*
 * resize_overlay.h - Temporary dimension overlay on pane resize
 *
 * Shows a floating label with pane dimensions (e.g. "640x480 | 640x480")
 * when a GtkPaned handle is dragged.  Auto-hides after 1200ms.
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * resize_overlay_init:
 * @overlay: the GtkOverlay that wraps the main layout.
 *
 * Must be called once to set the overlay where dimension labels appear.
 */
void resize_overlay_init(GtkOverlay *overlay);

/*
 * resize_overlay_connect_paned:
 * @paned: a GtkPaned widget to monitor for position changes.
 *
 * Connects to "notify::position" so that dragging the handle
 * shows the resize overlay.  Safe to call multiple times for
 * different paned widgets.
 */
void resize_overlay_connect_paned(GtkPaned *paned);

G_END_DECLS
