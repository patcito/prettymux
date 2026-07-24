#include "desktop_toolbar.h"

#include "app_actions.h"
#include "app_state.h"
#include "workspace_picker.h"

static void
toolbar_action_clicked(GtkButton *button, gpointer user_data)
{
    const char *action = user_data;

    (void)button;
    app_actions_handle(action);
}

static void
toolbar_project_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    (void)user_data;
    workspace_picker_show(g_main_window);
}

static GtkWidget *
toolbar_button(const char *label, const char *action)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    gtk_widget_set_focusable(button, FALSE);
    g_object_set_data_full(G_OBJECT(button), "prettymux-action",
                           g_strdup(action), g_free);
    g_signal_connect(button, "clicked",
                     G_CALLBACK(toolbar_action_clicked),
                     g_object_get_data(G_OBJECT(button), "prettymux-action"));
    return button;
}

GtkWidget *
desktop_toolbar_new(void)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *project = gtk_button_new_with_label("New Project…");

    gtk_widget_add_css_class(toolbar, "toolbar");
    gtk_widget_add_css_class(toolbar, "desktop-toolbar");
    gtk_widget_set_margin_start(toolbar, 6);
    gtk_widget_set_margin_end(toolbar, 6);
    gtk_widget_set_margin_top(toolbar, 4);
    gtk_widget_set_margin_bottom(toolbar, 4);

    gtk_widget_set_focusable(project, FALSE);
    g_object_set_data(G_OBJECT(project), "prettymux-role", "new-project");
    g_signal_connect(project, "clicked",
                     G_CALLBACK(toolbar_project_clicked), NULL);
    gtk_box_append(GTK_BOX(toolbar), project);

    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("New Tab", "pane.tab.new"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("Right Terminal", "split.horizontal"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("Below Terminal", "split.vertical"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("Close Pane", "pane.close"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("Zoom / Restore", "pane.zoom"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("All Actions", "search.show"));

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), toolbar);
    gtk_widget_set_vexpand(scroll, FALSE);
    g_object_set_data(G_OBJECT(scroll), "prettymux-toolbar", toolbar);
    return scroll;
}
