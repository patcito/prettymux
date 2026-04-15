#include "workspace_layout.h"
#include "workspace.h"
#include "workspace_strip.h"

static void
workspace_rebuild_add_saved_notebook(GPtrArray *saved, GtkWidget *notebook)
{
    if (!saved || !GTK_IS_NOTEBOOK(notebook))
        return;

    for (guint i = 0; i < saved->len; i++) {
        if (g_ptr_array_index(saved, i) == notebook)
            return;
    }

    g_object_ref(notebook);
    g_ptr_array_add(saved, notebook);
}

static void
workspace_rebuild_collect_notebooks(GtkWidget *widget, GPtrArray *saved)
{
    if (!widget || !saved)
        return;

    if (GTK_IS_NOTEBOOK(widget))
        workspace_rebuild_add_saved_notebook(saved, widget);

    for (GtkWidget *child = gtk_widget_get_first_child(widget);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        workspace_rebuild_collect_notebooks(child, saved);
    }
}

WorkspaceLayoutMode
workspace_get_layout_mode(Workspace *ws)
{
    if (!ws)
        return WORKSPACE_LAYOUT_CLASSIC;
    return ws->layout_mode;
}

gboolean
workspace_rebuild_for_layout_mode(Workspace *ws, WorkspaceLayoutMode mode)
{
    if (!ws)
        return FALSE;
    if (ws->layout_mode == mode)
        return TRUE;
    if (!ws->overlay || !ws->notebook)
        return FALSE;

    GPtrArray *saved = g_ptr_array_new();

    workspace_rebuild_collect_notebooks(
        gtk_overlay_get_child(GTK_OVERLAY(ws->overlay)), saved);

    if (saved->len == 0 && ws->pane_notebooks) {
        for (guint i = 0; i < ws->pane_notebooks->len; i++) {
            GtkWidget *nb = g_ptr_array_index(ws->pane_notebooks, i);
            workspace_rebuild_add_saved_notebook(saved, nb);
        }
    }

    workspace_rebuild_add_saved_notebook(saved, ws->notebook);
    if (saved->len == 0) {
        g_ptr_array_free(saved, TRUE);
        return FALSE;
    }

    gboolean has_primary = FALSE;
    for (guint i = 0; i < saved->len; i++) {
        if (g_ptr_array_index(saved, i) == ws->notebook) {
            has_primary = TRUE;
            if (i > 0) {
                gpointer tmp = g_ptr_array_index(saved, i);
                g_ptr_array_index(saved, i) = g_ptr_array_index(saved, 0);
                g_ptr_array_index(saved, 0) = tmp;
            }
            break;
        }
    }
    if (!has_primary)
        ws->notebook = g_ptr_array_index(saved, 0);

    if (ws->strip_state) {
        workspace_strip_state_free(ws->strip_state);
        ws->strip_state = NULL;
    }
    gtk_overlay_set_child(GTK_OVERLAY(ws->overlay), NULL);

    ws->layout_mode = mode;
    GtkWidget *first_nb = g_ptr_array_index(saved, 0);
    GtkWidget *new_root = workspace_layout_create_root(ws, first_nb);

    if (mode == WORKSPACE_LAYOUT_STRIP && saved->len > 1) {
        for (guint i = 1; i < saved->len; i++)
            workspace_strip_add_notebook_column(
                ws, g_ptr_array_index(saved, i));
    } else if (mode == WORKSPACE_LAYOUT_CLASSIC && saved->len > 1) {
        for (guint i = 1; i < saved->len; i++) {
            GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_set_hexpand(paned, TRUE);
            gtk_widget_set_vexpand(paned, TRUE);
            gtk_paned_set_start_child(GTK_PANED(paned), new_root);
            gtk_paned_set_end_child(GTK_PANED(paned),
                                    g_ptr_array_index(saved, i));
            gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
            gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
            gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
            gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
            new_root = paned;
        }
    }

    gtk_overlay_set_child(GTK_OVERLAY(ws->overlay), new_root);

    if (ws->pane_notebooks)
        g_ptr_array_set_size(ws->pane_notebooks, 0);
    else
        ws->pane_notebooks = g_ptr_array_new();
    for (guint i = 0; i < saved->len; i++) {
        g_ptr_array_add(ws->pane_notebooks, g_ptr_array_index(saved, i));
    }
    ws->notebook = g_ptr_array_index(saved, 0);
    if (!ws->active_pane || workspace_get_pane_index(ws, ws->active_pane) < 0)
        ws->active_pane = GTK_NOTEBOOK(ws->notebook);

    for (guint i = 0; i < saved->len; i++)
        g_object_unref(g_ptr_array_index(saved, i));
    g_ptr_array_free(saved, TRUE);

    workspace_layout_focus_primary(ws);
    return TRUE;
}

void
workspace_set_layout_mode(Workspace *ws, WorkspaceLayoutMode mode)
{
    if (!ws)
        return;
    ws->layout_mode = mode;
}

GtkWidget *
workspace_layout_create_root(Workspace *ws, GtkWidget *first_notebook)
{
    if (!ws || !first_notebook)
        return first_notebook;

    switch (ws->layout_mode) {
    case WORKSPACE_LAYOUT_STRIP:
        return workspace_strip_create_root(ws, first_notebook);
    case WORKSPACE_LAYOUT_CLASSIC:
    default:
        return first_notebook;
    }
}

void
workspace_layout_focus_primary(Workspace *ws)
{
    if (!ws)
        return;

    switch (ws->layout_mode) {
    case WORKSPACE_LAYOUT_STRIP:
        workspace_strip_focus_primary(ws);
        break;
    case WORKSPACE_LAYOUT_CLASSIC:
    default:
        workspace_focus_first_terminal(ws);
        break;
    }
}

void
workspace_layout_toggle_zoom_current(Workspace *ws)
{
    if (!ws)
        return;

    switch (ws->layout_mode) {
    case WORKSPACE_LAYOUT_STRIP:
        workspace_strip_toggle_zoom(ws);
        break;
    case WORKSPACE_LAYOUT_CLASSIC:
    default:
        workspace_toggle_zoom(ws);
        break;
    }
}

gboolean
workspace_layout_is_zoomed(Workspace *ws)
{
    if (!ws)
        return FALSE;

    switch (ws->layout_mode) {
    case WORKSPACE_LAYOUT_STRIP:
        if (ws->strip_state && ws->strip_state->columns) {
            int fc = ws->strip_state->focused_col;
            if (fc >= 0 && fc < (int)ws->strip_state->columns->len) {
                WorkspaceColumn *col =
                    g_ptr_array_index(ws->strip_state->columns, fc);
                return col->maximized;
            }
        }
        return FALSE;
    case WORKSPACE_LAYOUT_CLASSIC:
    default:
        return ws->zoomed;
    }
}
