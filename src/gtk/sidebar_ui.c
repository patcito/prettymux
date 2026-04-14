#include "sidebar_ui.h"

#include <gtk/gtk.h>
#include <string.h>

#include "app_state.h"
#include "notifications.h"
#include "session.h"
#include "workspace.h"

static void
on_workspace_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    GtkWidget *child;
    GtkWidget *rename_entry;

    (void)list;
    (void)user_data;

    child = gtk_list_box_row_get_child(row);
    rename_entry = child ? g_object_get_data(G_OBJECT(child), "rename-entry") : NULL;
    if (GTK_IS_WIDGET(rename_entry)) {
        gtk_widget_grab_focus(rename_entry);
        return;
    }

    workspace_switch(gtk_list_box_row_get_index(row),
                     ui.terminal_stack, ui.workspace_list);
    session_queue_save();
}

static gboolean
workspace_row_matches_query(Workspace *ws, const char *query)
{
    g_autofree char *needle = NULL;
    g_autofree char *name = NULL;
    g_autofree char *cwd = NULL;
    g_autofree char *branch = NULL;

    if (!ws || !query || !query[0])
        return TRUE;

    needle = g_utf8_strdown(query, -1);
    name = g_utf8_strdown(ws->name, -1);
    cwd = g_utf8_strdown(ws->cwd, -1);
    branch = g_utf8_strdown(ws->git_branch, -1);

    return (name && strstr(name, needle)) ||
           (cwd && strstr(cwd, needle)) ||
           (branch && strstr(branch, needle));
}

static gboolean
workspace_list_filter_func(GtkListBoxRow *row, gpointer user_data)
{
    GtkWidget *search = GTK_WIDGET(user_data);
    GtkWidget *child;
    Workspace *ws;
    const char *query;

    if (!GTK_IS_EDITABLE(search))
        return TRUE;

    query = gtk_editable_get_text(GTK_EDITABLE(search));
    if (!query || !query[0])
        return TRUE;

    child = gtk_list_box_row_get_child(row);
    ws = child ? g_object_get_data(G_OBJECT(child), "workspace") : NULL;
    return workspace_row_matches_query(ws, query);
}

static void
on_workspace_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    GtkListBox *list = GTK_LIST_BOX(user_data);
    (void)entry;

    if (GTK_IS_LIST_BOX(list))
        gtk_list_box_invalidate_filter(list);
}

static void
on_add_workspace_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    workspace_add(ui.terminal_stack, ui.workspace_list, g_ghostty_app);
}

void
sidebar_ui_build(void)
{
    GtkWidget *bottom_box;
    GtkWidget *scroll;
    GtkWidget *btn;

    ui.sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(ui.sidebar_box, "sidebar");
    gtk_widget_set_size_request(ui.sidebar_box, 180, -1);

    ui.workspace_search = gtk_search_entry_new();
    g_object_set(G_OBJECT(ui.workspace_search),
                 "placeholder-text", "Search workspaces",
                 NULL);
    gtk_widget_set_margin_start(ui.workspace_search, 8);
    gtk_widget_set_margin_end(ui.workspace_search, 8);
    gtk_widget_set_margin_top(ui.workspace_search, 8);
    gtk_widget_set_margin_bottom(ui.workspace_search, 4);
    gtk_box_append(GTK_BOX(ui.sidebar_box), ui.workspace_search);

    ui.workspace_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui.workspace_list),
                                    GTK_SELECTION_SINGLE);
    g_signal_connect(ui.workspace_list, "row-activated",
                     G_CALLBACK(on_workspace_row_activated), NULL);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(ui.workspace_list),
                                 workspace_list_filter_func,
                                 ui.workspace_search, NULL);
    g_signal_connect(ui.workspace_search, "search-changed",
                     G_CALLBACK(on_workspace_search_changed),
                     ui.workspace_list);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui.workspace_list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(ui.sidebar_box), scroll);

    bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(bottom_box, 8);
    gtk_widget_set_margin_end(bottom_box, 8);
    gtk_widget_set_margin_bottom(bottom_box, 8);
    gtk_widget_set_margin_top(bottom_box, 4);

    ui.bell_button = gtk_button_new_with_label("\360\237\224\224");
    g_signal_connect(ui.bell_button, "clicked",
                     G_CALLBACK(notifications_on_bell_button_clicked), NULL);
    gtk_box_append(GTK_BOX(bottom_box), ui.bell_button);

    btn = gtk_button_new_with_label("+ New Workspace");
    gtk_widget_set_hexpand(btn, TRUE);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_add_workspace_clicked), NULL);
    gtk_box_append(GTK_BOX(bottom_box), btn);

    gtk_box_append(GTK_BOX(ui.sidebar_box), bottom_box);
}
