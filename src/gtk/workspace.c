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

void workspace_add_terminal(Workspace *ws, ghostty_app_t app) {
    (void)app;
    GtkWidget *terminal = ghostty_terminal_new(NULL);
    g_ptr_array_add(ws->terminals, terminal);

    GtkWidget *label = gtk_label_new("Terminal");
    gtk_notebook_append_page(GTK_NOTEBOOK(ws->notebook), terminal, label);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(ws->notebook), terminal, TRUE);

    g_signal_connect(terminal, "title-changed", G_CALLBACK(on_title_changed), label);

    gtk_widget_set_visible(terminal, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(ws->notebook),
        gtk_notebook_get_n_pages(GTK_NOTEBOOK(ws->notebook)) - 1);
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

void workspace_add(GtkWidget *terminal_stack, GtkWidget *workspace_list, ghostty_app_t app) {
    if (!workspaces)
        workspaces = g_ptr_array_new();

    Workspace *ws = g_new0(Workspace, 1);
    snprintf(ws->name, sizeof(ws->name), "Workspace %d", (int)workspaces->len + 1);
    ws->terminals = g_ptr_array_new();

    // Create notebook for terminal tabs
    ws->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(ws->notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(ws->notebook), FALSE);

    // "+" button
    GtkWidget *add_btn = gtk_button_new_with_label("+");
    g_object_set_data(G_OBJECT(add_btn), "workspace", ws);
    g_object_set_data(G_OBJECT(add_btn), "app", app);
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_ws_add_tab_clicked), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(ws->notebook), add_btn, GTK_PACK_END);
    gtk_widget_set_visible(add_btn, TRUE);

    ws->container = ws->notebook;

    // Add to stack
    char stack_name[32];
    snprintf(stack_name, sizeof(stack_name), "ws-%d", (int)workspaces->len);
    gtk_stack_add_named(GTK_STACK(terminal_stack), ws->container, stack_name);

    // Add to sidebar
    GtkWidget *row = create_workspace_row(ws);
    gtk_list_box_append(GTK_LIST_BOX(workspace_list), row);

    g_ptr_array_add(workspaces, ws);

    // Create first terminal
    workspace_add_terminal(ws, app);

    // Switch to it
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
