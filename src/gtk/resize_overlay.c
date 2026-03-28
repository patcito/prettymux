/*
 * resize_overlay.c - Temporary dimension overlay on pane resize
 */

#include "resize_overlay.h"
#include <stdio.h>

/* ── Module state ────────────────────────────────────────────────── */

static GtkOverlay *g_overlay = NULL;
static GtkWidget  *g_label   = NULL;
static guint       g_hide_id = 0;

/* ── Hide callback ───────────────────────────────────────────────── */

static gboolean
hide_overlay_label(gpointer data)
{
    (void)data;
    if (g_label) {
        gtk_widget_set_visible(g_label, FALSE);
    }
    g_hide_id = 0;
    return G_SOURCE_REMOVE;
}

/* ── Position change callback ────────────────────────────────────── */

static void
on_paned_position_changed(GObject    *object,
                           GParamSpec *pspec,
                           gpointer    user_data)
{
    (void)pspec;
    (void)user_data;

    GtkPaned *paned = GTK_PANED(object);
    if (!g_overlay)
        return;

    /* Get the two children and their sizes */
    GtkWidget *start_child = gtk_paned_get_start_child(paned);
    GtkWidget *end_child   = gtk_paned_get_end_child(paned);
    if (!start_child || !end_child)
        return;

    int sw = gtk_widget_get_width(start_child);
    int sh = gtk_widget_get_height(start_child);
    int ew = gtk_widget_get_width(end_child);
    int eh = gtk_widget_get_height(end_child);

    /* Format: "WxH  |  WxH" */
    char text[128];
    snprintf(text, sizeof(text), "%dx%d  |  %dx%d", sw, sh, ew, eh);

    /* Create or reuse the overlay label */
    if (!g_label) {
        g_label = gtk_label_new(text);
        gtk_widget_set_halign(g_label, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(g_label, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(g_label, "resize-overlay");
        gtk_overlay_add_overlay(g_overlay, g_label);
    } else {
        gtk_label_set_text(GTK_LABEL(g_label), text);
    }

    gtk_widget_set_visible(g_label, TRUE);

    /* Reset the auto-hide timer */
    if (g_hide_id)
        g_source_remove(g_hide_id);
    g_hide_id = g_timeout_add(1200, hide_overlay_label, NULL);
}

/* ── Public API ──────────────────────────────────────────────────── */

void
resize_overlay_init(GtkOverlay *overlay)
{
    g_overlay = overlay;
}

void
resize_overlay_connect_paned(GtkPaned *paned)
{
    g_return_if_fail(GTK_IS_PANED(paned));
    g_signal_connect(paned, "notify::position",
                     G_CALLBACK(on_paned_position_changed), NULL);
}
