#include "workspace.h"
#include "ghostty_terminal.h"
#include <stdio.h>

GPtrArray *workspaces = NULL;
int current_workspace = 0;

Workspace *workspace_get_current(void) {
    if (!workspaces || current_workspace >= (int)workspaces->len)
        return NULL;
    return g_ptr_array_index(workspaces, current_workspace);
}

static void on_title_changed(GhosttyTerminal *term, const char *title, gpointer label_ptr) {
    (void)term;
    char short_title[32];
    snprintf(short_title, sizeof(short_title), "%.28s", title);
    gtk_label_set_text(GTK_LABEL(label_ptr), short_title);
}

/* Add a terminal tab to a specific notebook within a workspace. */
static void
workspace_add_terminal_to_notebook(Workspace *ws, GtkNotebook *notebook,
                                   ghostty_app_t app)
{
    (void)app;
    GtkWidget *terminal = ghostty_terminal_new(NULL);
    g_ptr_array_add(ws->terminals, terminal);

    GtkWidget *label = gtk_label_new("Terminal");
    gtk_notebook_append_page(notebook, terminal, label);
    gtk_notebook_set_tab_reorderable(notebook, terminal, TRUE);

    g_signal_connect(terminal, "title-changed", G_CALLBACK(on_title_changed), label);

    gtk_widget_set_visible(terminal, TRUE);
    gtk_notebook_set_current_page(notebook,
        gtk_notebook_get_n_pages(notebook) - 1);
}

void workspace_add_terminal(Workspace *ws, ghostty_app_t app) {
    /* Add to the first pane notebook (backwards compat). */
    workspace_add_terminal_to_notebook(ws, GTK_NOTEBOOK(ws->notebook), app);
}

void workspace_add_terminal_to_focused(Workspace *ws, ghostty_app_t app) {
    GtkNotebook *focused = workspace_get_focused_pane(ws);
    if (focused)
        workspace_add_terminal_to_notebook(ws, focused, app);
    else
        workspace_add_terminal(ws, app);
}

static GtkWidget *create_workspace_row(Workspace *ws) {
    GtkWidget *label = gtk_label_new(ws->name);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "sidebar-row");
    return label;
}

static void on_ws_add_tab_clicked(GtkButton *btn, gpointer data) {
    (void)data;
    Workspace *w = g_object_get_data(G_OBJECT(btn), "workspace");
    ghostty_app_t a = g_object_get_data(G_OBJECT(btn), "app");
    workspace_add_terminal(w, a);
}

/* Helper: create a notebook for a new pane and wire up the "+" button. */
static GtkWidget *
create_pane_notebook(Workspace *ws, ghostty_app_t app)
{
    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);

    /* "+" button */
    GtkWidget *add_btn = gtk_button_new_with_label("+");
    g_object_set_data(G_OBJECT(add_btn), "workspace", ws);
    g_object_set_data(G_OBJECT(add_btn), "app", app);
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_ws_add_tab_clicked), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), add_btn, GTK_PACK_END);
    gtk_widget_set_visible(add_btn, TRUE);

    return notebook;
}

void workspace_add(GtkWidget *terminal_stack, GtkWidget *workspace_list, ghostty_app_t app) {
    if (!workspaces)
        workspaces = g_ptr_array_new();

    Workspace *ws = g_new0(Workspace, 1);
    snprintf(ws->name, sizeof(ws->name), "Workspace %d", (int)workspaces->len + 1);
    ws->terminals = g_ptr_array_new();
    ws->pane_notebooks = g_ptr_array_new();

    /* Create the first pane notebook */
    ws->notebook = create_pane_notebook(ws, app);
    g_ptr_array_add(ws->pane_notebooks, ws->notebook);

    ws->container = ws->notebook;

    /* Add to stack */
    char stack_name[32];
    snprintf(stack_name, sizeof(stack_name), "ws-%d", (int)workspaces->len);
    gtk_stack_add_named(GTK_STACK(terminal_stack), ws->container, stack_name);

    /* Add to sidebar */
    GtkWidget *row = create_workspace_row(ws);
    gtk_list_box_append(GTK_LIST_BOX(workspace_list), row);

    g_ptr_array_add(workspaces, ws);

    /* Create first terminal */
    workspace_add_terminal(ws, app);

    /* Switch to it */
    workspace_switch(workspaces->len - 1, terminal_stack, workspace_list);
}

void workspace_remove(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list) {
    if (!workspaces || workspaces->len <= 1 || index >= (int)workspaces->len)
        return;

    Workspace *ws = g_ptr_array_index(workspaces, index);
    gtk_stack_remove(GTK_STACK(terminal_stack), ws->container);

    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), index);
    if (row) gtk_list_box_remove(GTK_LIST_BOX(workspace_list), GTK_WIDGET(row));

    g_ptr_array_remove_index(workspaces, index);
    if (current_workspace >= (int)workspaces->len)
        current_workspace = workspaces->len - 1;

    workspace_switch(current_workspace, terminal_stack, workspace_list);

    g_ptr_array_unref(ws->terminals);
    g_ptr_array_unref(ws->pane_notebooks);
    g_free(ws);
}

void workspace_switch(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list) {
    if (!workspaces || index < 0 || index >= (int)workspaces->len)
        return;
    current_workspace = index;

    char stack_name[32];
    snprintf(stack_name, sizeof(stack_name), "ws-%d", index);
    gtk_stack_set_visible_child_name(GTK_STACK(terminal_stack), stack_name);

    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), index);
    if (row)
        gtk_list_box_select_row(GTK_LIST_BOX(workspace_list), row);
}

/* ── Pane splitting ───────────────────────────────────────────── */

/*
 * workspace_get_focused_pane:
 *
 * Walk the workspace's pane notebooks and find which one contains the
 * widget that currently has keyboard focus.  Falls back to the first
 * notebook.
 */
GtkNotebook *
workspace_get_focused_pane(Workspace *ws)
{
    if (!ws || !ws->pane_notebooks || ws->pane_notebooks->len == 0)
        return NULL;

    GtkRoot *root = NULL;
    GtkWidget *focus = NULL;

    /* Try to find the focused widget */
    GtkNotebook *first_nb = g_ptr_array_index(ws->pane_notebooks, 0);
    root = gtk_widget_get_root(GTK_WIDGET(first_nb));
    if (root)
        focus = gtk_root_get_focus(root);

    if (focus) {
        /* Walk up the focus widget's ancestors to find which notebook it
         * belongs to. */
        for (GtkWidget *w = focus; w != NULL; w = gtk_widget_get_parent(w)) {
            if (GTK_IS_NOTEBOOK(w)) {
                /* Verify this notebook is one of our pane notebooks */
                for (guint i = 0; i < ws->pane_notebooks->len; i++) {
                    if (g_ptr_array_index(ws->pane_notebooks, i) == w)
                        return GTK_NOTEBOOK(w);
                }
            }
        }
    }

    return first_nb;
}

/*
 * workspace_split_pane:
 *
 * Splits the currently focused pane notebook.  The notebook is
 * replaced in its parent by a GtkPaned; the original notebook
 * becomes start_child and a new notebook becomes end_child.
 *
 * If the notebook is the direct workspace container (no splits
 * yet), we replace ws->container in the GtkStack.
 */
void
workspace_split_pane(Workspace *ws, GtkOrientation orientation,
                     ghostty_app_t app)
{
    if (!ws) return;

    GtkNotebook *source_nb = workspace_get_focused_pane(ws);
    if (!source_nb) return;

    GtkWidget *source_widget = GTK_WIDGET(source_nb);
    GtkWidget *parent = gtk_widget_get_parent(source_widget);

    /* Create the new pane notebook */
    GtkWidget *new_nb = create_pane_notebook(ws, app);
    g_ptr_array_add(ws->pane_notebooks, new_nb);

    /* Create the new paned container */
    GtkWidget *paned = gtk_paned_new(orientation);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_vexpand(paned, TRUE);

    if (GTK_IS_PANED(parent)) {
        /*
         * The source notebook is already inside a GtkPaned.
         * Determine if it is the start or end child, then replace it
         * with the new paned.
         */
        GtkWidget *start = gtk_paned_get_start_child(GTK_PANED(parent));

        /* We need to unparent the source first.  GtkPaned doesn't have
         * a replace API, so we set_start/end_child(NULL), build the
         * new paned, then put it back. */
        if (start == source_widget) {
            gtk_paned_set_start_child(GTK_PANED(parent), NULL);
            gtk_paned_set_start_child(GTK_PANED(paned), source_widget);
            gtk_paned_set_end_child(GTK_PANED(paned), new_nb);
            gtk_paned_set_start_child(GTK_PANED(parent), paned);
        } else {
            gtk_paned_set_end_child(GTK_PANED(parent), NULL);
            gtk_paned_set_start_child(GTK_PANED(paned), source_widget);
            gtk_paned_set_end_child(GTK_PANED(paned), new_nb);
            gtk_paned_set_end_child(GTK_PANED(parent), paned);
        }

        gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
        gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);

    } else {
        /*
         * The source notebook is the direct child of the GtkStack page.
         * We need to remove it from the stack and insert the new paned
         * in its place.
         */
        GtkWidget *stack = parent;  /* Should be the GtkStack */

        /* Determine the stack page name before removal */
        int ws_idx = -1;
        for (guint i = 0; i < workspaces->len; i++) {
            if (g_ptr_array_index(workspaces, i) == ws) {
                ws_idx = (int)i;
                break;
            }
        }

        /* Ref the source so it survives removal from the stack */
        g_object_ref(source_widget);
        gtk_stack_remove(GTK_STACK(stack), source_widget);

        gtk_paned_set_start_child(GTK_PANED(paned), source_widget);
        gtk_paned_set_end_child(GTK_PANED(paned), new_nb);
        gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
        gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);

        g_object_unref(source_widget);

        /* Add the paned to the stack */
        char stack_name[32];
        snprintf(stack_name, sizeof(stack_name), "ws-%d",
                 ws_idx >= 0 ? ws_idx : 0);
        gtk_stack_add_named(GTK_STACK(stack), paned, stack_name);
        gtk_stack_set_visible_child(GTK_STACK(stack), paned);

        ws->container = paned;
    }

    gtk_widget_set_visible(paned, TRUE);
    gtk_widget_set_visible(new_nb, TRUE);

    /* Add a terminal to the new pane */
    workspace_add_terminal_to_notebook(ws, GTK_NOTEBOOK(new_nb), app);

    /* Focus the new terminal */
    int last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(new_nb)) - 1;
    if (last >= 0) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(new_nb), last);
        if (page)
            gtk_widget_grab_focus(page);
    }
}

/*
 * workspace_close_pane:
 *
 * Remove a pane notebook and collapse the GtkPaned that contained it.
 * The sibling takes the paned's place in the widget tree.
 *
 * If this was the last pane, do nothing.
 */
void
workspace_close_pane(Workspace *ws, GtkNotebook *pane)
{
    if (!ws || !pane) return;
    if (!ws->pane_notebooks || ws->pane_notebooks->len <= 1) return;

    GtkWidget *pane_widget = GTK_WIDGET(pane);
    GtkWidget *parent = gtk_widget_get_parent(pane_widget);

    if (!GTK_IS_PANED(parent)) return;

    GtkPaned *parent_paned = GTK_PANED(parent);
    GtkWidget *grandparent = gtk_widget_get_parent(GTK_WIDGET(parent_paned));

    /* Determine the sibling (the other child of the paned). */
    GtkWidget *start = gtk_paned_get_start_child(parent_paned);
    GtkWidget *sibling = (start == pane_widget)
        ? gtk_paned_get_end_child(parent_paned)
        : start;

    if (!sibling) return;

    /* Remove terminals that belong to the closing pane from ws->terminals */
    int n_pages = gtk_notebook_get_n_pages(pane);
    for (int i = 0; i < n_pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(pane, i);
        g_ptr_array_remove(ws->terminals, child);
    }

    /* Remove the pane from pane_notebooks */
    g_ptr_array_remove(ws->pane_notebooks, pane);

    /* Unparent both children from the paned */
    g_object_ref(sibling);
    gtk_paned_set_start_child(parent_paned, NULL);
    gtk_paned_set_end_child(parent_paned, NULL);

    /* Replace the paned with the sibling in its grandparent */
    if (GTK_IS_PANED(grandparent)) {
        GtkWidget *gp_start = gtk_paned_get_start_child(GTK_PANED(grandparent));
        if (gp_start == GTK_WIDGET(parent_paned)) {
            gtk_paned_set_start_child(GTK_PANED(grandparent), NULL);
            gtk_paned_set_start_child(GTK_PANED(grandparent), sibling);
        } else {
            gtk_paned_set_end_child(GTK_PANED(grandparent), NULL);
            gtk_paned_set_end_child(GTK_PANED(grandparent), sibling);
        }
    } else if (GTK_IS_STACK(grandparent)) {
        /* The paned was the direct child of the stack */
        int ws_idx = -1;
        for (guint i = 0; i < workspaces->len; i++) {
            if (g_ptr_array_index(workspaces, i) == ws) {
                ws_idx = (int)i;
                break;
            }
        }

        gtk_stack_remove(GTK_STACK(grandparent), GTK_WIDGET(parent_paned));

        char stack_name[32];
        snprintf(stack_name, sizeof(stack_name), "ws-%d",
                 ws_idx >= 0 ? ws_idx : 0);
        gtk_stack_add_named(GTK_STACK(grandparent), sibling, stack_name);
        gtk_stack_set_visible_child(GTK_STACK(grandparent), sibling);

        ws->container = sibling;

        /* If the sibling is a notebook, it becomes the primary again */
        if (GTK_IS_NOTEBOOK(sibling))
            ws->notebook = sibling;
    }

    g_object_unref(sibling);
}
