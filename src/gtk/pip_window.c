/*
 * pip_window.c - Picture-in-Picture floating browser window
 *
 * Creates a separate undecorated always-on-top GtkWindow containing
 * a reparented WebKitWebView from the current browser tab.  Custom
 * title bar with drag-to-move and close button.
 */

#include "pip_window.h"
#include "browser_tab.h"

#include <string.h>
#include <webkit/webkit.h>

/* ── PipWindow structure ─────────────────────────────────────── */

struct PipWindow {
    GtkWindow    *window;
    GtkWidget    *title_label;

    /* Original location for restoring the view */
    WebKitWebView *web_view;
    GtkWidget     *original_tab;      /* BrowserTab widget */
    int            original_tab_idx;
    GtkWidget     *browser_notebook;

};

/* Global singleton */
static PipWindow *g_pip = NULL;

/* ── Callbacks ───────────────────────────────────────────────── */

static void
on_pip_close_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    PipWindow *pip = user_data;
    pip_window_close(pip);
}

static gboolean
on_pip_close_request(GtkWindow *window, gpointer user_data)
{
    (void)window;
    PipWindow *pip = user_data;
    pip_window_close(pip);
    return TRUE; /* We handle destruction ourselves */
}

static gboolean
on_pip_key_pressed(GtkEventControllerKey *controller,
                   guint                  keyval,
                   guint                  keycode,
                   GdkModifierType        state,
                   gpointer               user_data)
{
    (void)controller;
    (void)keycode;
    PipWindow *pip = user_data;

    if (keyval == GDK_KEY_Escape ||
        (keyval == GDK_KEY_m &&
         (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) ==
             (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) ||
        (keyval == GDK_KEY_w && (state & GDK_CONTROL_MASK))) {
        pip_window_close(pip);
        return TRUE;
    }

    return FALSE;
}

static void
on_title_bar_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                     gpointer user_data)
{
    (void)n_press; (void)x; (void)y;
    PipWindow *pip = user_data;

    /* Initiate an interactive move via the Wayland/X11 compositor */
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(pip->window));
    if (surface && GDK_IS_TOPLEVEL(surface)) {
        GdkDevice *device = gtk_gesture_get_device(GTK_GESTURE(gesture));
        GdkEvent *event = gtk_event_controller_get_current_event(
            GTK_EVENT_CONTROLLER(gesture));
        guint32 timestamp = event ? gdk_event_get_time(event) : GDK_CURRENT_TIME;

        gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), device, 0, x, y, timestamp);
    }
}

/* ── Public API ──────────────────────────────────────────────── */

PipWindow *
pip_window_new(GtkWindow *parent, GtkWidget *browser_notebook)
{
    if (g_pip)
        return NULL; /* Already have one */

    if (!browser_notebook)
        return NULL;

    int page_idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(browser_notebook));
    if (page_idx < 0)
        return NULL;

    GtkWidget *tab_widget = gtk_notebook_get_nth_page(
        GTK_NOTEBOOK(browser_notebook), page_idx);
    if (!tab_widget || !BROWSER_IS_TAB(tab_widget))
        return NULL;

    /* Get the WebKitWebView from the BrowserTab.
     * BrowserTab is a GtkBox; the web view is a child. */
    WebKitWebView *web_view = NULL;
    {
        GtkWidget *child = gtk_widget_get_first_child(tab_widget);
        while (child) {
            if (WEBKIT_IS_WEB_VIEW(child)) {
                web_view = WEBKIT_WEB_VIEW(child);
                break;
            }
            child = gtk_widget_get_next_sibling(child);
        }
    }
    if (!web_view)
        return NULL;

    PipWindow *pip = g_new0(PipWindow, 1);
    pip->web_view = web_view;
    pip->original_tab = tab_widget;
    pip->original_tab_idx = page_idx;
    pip->browser_notebook = browser_notebook;

    /* Create the window */
    pip->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(pip->window, "Picture in Picture");
    gtk_window_set_default_size(pip->window, 560, 380);
    gtk_window_set_decorated(pip->window, FALSE);
    gtk_window_set_transient_for(pip->window, parent);

    /* Try to keep on top (only works on some WMs) */
    /* GTK4 removed always-on-top; we do our best with transient_for. */

    /* Build layout: title bar + web view area */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(pip->window, vbox);

    /* Title bar */
    GtkWidget *title_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(title_bar, "toolbar");
    gtk_widget_set_size_request(title_bar, -1, 32);
    gtk_widget_set_margin_start(title_bar, 8);
    gtk_widget_set_margin_end(title_bar, 4);
    gtk_box_append(GTK_BOX(vbox), title_bar);

    /* Title label */
    const char *view_title = webkit_web_view_get_title(web_view);
    pip->title_label = gtk_label_new(
        (view_title && *view_title) ? view_title : "Picture in Picture");
    gtk_label_set_ellipsize(GTK_LABEL(pip->title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(pip->title_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(pip->title_label), 0);
    gtk_box_append(GTK_BOX(title_bar), pip->title_label);

    /* Close button */
    GtkWidget *close_btn = gtk_button_new_with_label("×");
    gtk_widget_set_size_request(close_btn, 24, 24);
    g_signal_connect(close_btn, "clicked",
                     G_CALLBACK(on_pip_close_clicked), pip);
    gtk_box_append(GTK_BOX(title_bar), close_btn);

    /* Drag gesture on the title bar for moving the window */
    GtkGesture *click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), 1);
    g_signal_connect(click_gesture, "pressed",
                     G_CALLBACK(on_title_bar_pressed), pip);
    gtk_widget_add_controller(title_bar, GTK_EVENT_CONTROLLER(click_gesture));

    /* Reparent the web view: remove from BrowserTab, add to PiP */
    g_object_ref(GTK_WIDGET(web_view));
    gtk_box_remove(GTK_BOX(tab_widget), GTK_WIDGET(web_view));
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(web_view));
    gtk_widget_set_vexpand(GTK_WIDGET(web_view), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(web_view), TRUE);
    g_object_unref(GTK_WIDGET(web_view));

    /* Close request handler */
    g_signal_connect(pip->window, "close-request",
                     G_CALLBACK(on_pip_close_request), pip);

    /* Allow closing PiP without relying on the custom title bar button. */
    {
        GtkEventController *key_ctrl = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
        g_signal_connect(key_ctrl, "key-pressed",
                         G_CALLBACK(on_pip_key_pressed), pip);
        gtk_widget_add_controller(GTK_WIDGET(pip->window), key_ctrl);
    }

    g_pip = pip;
    gtk_window_present(pip->window);

    return pip;
}

void
pip_window_close(PipWindow *pip)
{
    if (!pip)
        return;

    if (pip->web_view && pip->original_tab) {
        /* Reparent the web view back to the original BrowserTab */
        g_object_ref(GTK_WIDGET(pip->web_view));

        /* Remove from PiP window's vbox */
        GtkWidget *vbox = gtk_window_get_child(pip->window);
        if (vbox)
            gtk_box_remove(GTK_BOX(vbox), GTK_WIDGET(pip->web_view));

        /* Add back to the BrowserTab (which is a GtkBox) */
        gtk_box_append(GTK_BOX(pip->original_tab), GTK_WIDGET(pip->web_view));
        gtk_widget_set_vexpand(GTK_WIDGET(pip->web_view), TRUE);
        gtk_widget_set_hexpand(GTK_WIDGET(pip->web_view), TRUE);

        g_object_unref(GTK_WIDGET(pip->web_view));
    }

    /* Destroy the window */
    gtk_window_destroy(pip->window);

    g_pip = NULL;
    g_free(pip);
}

gboolean
pip_window_is_active(void)
{
    return g_pip != NULL;
}

void
pip_window_toggle(GtkWindow *parent, GtkWidget *browser_notebook)
{
    if (g_pip) {
        pip_window_close(g_pip);
    } else {
        pip_window_new(parent, browser_notebook);
    }
}
