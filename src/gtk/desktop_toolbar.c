#include "desktop_toolbar.h"

#include "app_actions.h"
#include "app_state.h"
#include "sidebar_ui.h"
#include "ui_language.h"
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

static void
toolbar_language_changed(GtkDropDown *dropdown,
                         GParamSpec *pspec,
                         gpointer user_data)
{
    GtkWidget *toolbar = user_data;
    guint selected = gtk_drop_down_get_selected(dropdown);

    (void)pspec;
    if (selected >= UI_LANGUAGE_COUNT)
        return;
    ui_language_set((UiLanguage)selected);
    desktop_toolbar_update_language(toolbar);
    sidebar_ui_update_language();
}

static UiTextKey
toolbar_text_key_for_action(const char *action)
{
    if (g_strcmp0(action, "pane.tab.new") == 0)
        return UI_TEXT_NEW_TAB;
    if (g_strcmp0(action, "split.horizontal") == 0)
        return UI_TEXT_RIGHT_TERMINAL;
    if (g_strcmp0(action, "split.vertical") == 0)
        return UI_TEXT_BELOW_TERMINAL;
    if (g_strcmp0(action, "pane.close") == 0)
        return UI_TEXT_CLOSE_PANE;
    if (g_strcmp0(action, "pane.zoom") == 0)
        return UI_TEXT_ZOOM_RESTORE;
    return UI_TEXT_ALL_ACTIONS;
}

void
desktop_toolbar_update_language(GtkWidget *toolbar_root)
{
    GtkWidget *toolbar = g_object_get_data(
        G_OBJECT(toolbar_root), "prettymux-toolbar");
    GtkWidget *language_label = g_object_get_data(
        G_OBJECT(toolbar_root), "prettymux-language-label");
    GtkWidget *language_dropdown = g_object_get_data(
        G_OBJECT(toolbar_root), "prettymux-language-dropdown");

    if (!GTK_IS_BOX(toolbar))
        return;
    for (GtkWidget *child = gtk_widget_get_first_child(toolbar);
         child;
         child = gtk_widget_get_next_sibling(child)) {
        const char *role =
            g_object_get_data(G_OBJECT(child), "prettymux-role");
        const char *action =
            g_object_get_data(G_OBJECT(child), "prettymux-action");

        if (GTK_IS_BUTTON(child) &&
            g_strcmp0(role, "new-project") == 0) {
            gtk_button_set_label(GTK_BUTTON(child),
                                 ui_text(UI_TEXT_NEW_PROJECT));
        } else if (GTK_IS_BUTTON(child) && action) {
            gtk_button_set_label(
                GTK_BUTTON(child),
                ui_text(toolbar_text_key_for_action(action)));
        }
    }
    if (GTK_IS_LABEL(language_label))
        gtk_label_set_text(GTK_LABEL(language_label),
                           ui_text(UI_TEXT_LANGUAGE));
    if (GTK_IS_DROP_DOWN(language_dropdown))
        gtk_drop_down_set_selected(GTK_DROP_DOWN(language_dropdown),
                                   ui_language_current());
}

GtkWidget *
desktop_toolbar_new(void)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *project = gtk_button_new();
    GtkWidget *language_label = gtk_label_new(NULL);
    GtkWidget *language_dropdown;
    const char *language_names[UI_LANGUAGE_COUNT + 1] = {0};

    for (int i = 0; i < UI_LANGUAGE_COUNT; i++)
        language_names[i] = ui_language_display_name((UiLanguage)i);
    language_dropdown = gtk_drop_down_new_from_strings(language_names);

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
                   toolbar_button("", "pane.tab.new"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("", "split.horizontal"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("", "split.vertical"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("", "pane.close"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("", "pane.zoom"));
    gtk_box_append(GTK_BOX(toolbar),
                   toolbar_button("", "search.show"));

    gtk_widget_set_margin_start(language_label, 6);
    gtk_box_append(GTK_BOX(toolbar), language_label);
    gtk_box_append(GTK_BOX(toolbar), language_dropdown);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), toolbar);
    gtk_widget_set_vexpand(scroll, FALSE);
    g_object_set_data(G_OBJECT(scroll), "prettymux-toolbar", toolbar);
    g_object_set_data(G_OBJECT(scroll), "prettymux-language-label",
                      language_label);
    g_object_set_data(G_OBJECT(scroll), "prettymux-language-dropdown",
                      language_dropdown);
    g_signal_connect(language_dropdown, "notify::selected",
                     G_CALLBACK(toolbar_language_changed), scroll);
    desktop_toolbar_update_language(scroll);
    return scroll;
}
