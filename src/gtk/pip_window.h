/*
 * pip_window.h - Picture-in-Picture floating browser window
 *
 * Reparents a browser tab's WebKitWebView into a separate always-on-top
 * undecorated window.  On close, the view is returned to its original tab.
 */
#pragma once

#include <gtk/gtk.h>

typedef struct PipWindow PipWindow;

/*
 * pip_window_new:
 * @parent: the main application window (for positioning)
 * @browser_notebook: the GtkNotebook containing browser tabs
 *
 * Creates a PiP window for the currently active browser tab.
 * Returns NULL if no suitable tab is available.
 */
PipWindow *pip_window_new(GtkWindow *parent, GtkWidget *browser_notebook);

/*
 * pip_window_close:
 * @pip: the PiP window to close
 *
 * Restores the WebKitWebView to its original browser tab and
 * destroys the PiP window.
 */
void pip_window_close(PipWindow *pip);

/*
 * pip_window_is_active:
 *
 * Returns TRUE if there is currently an active PiP window.
 */
gboolean pip_window_is_active(void);

/*
 * pip_window_toggle:
 * @parent: the main application window
 * @browser_notebook: the browser GtkNotebook
 *
 * If a PiP window is showing, closes it.  Otherwise creates one
 * from the current browser tab.
 */
void pip_window_toggle(GtkWindow *parent, GtkWidget *browser_notebook);
