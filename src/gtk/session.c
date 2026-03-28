#include "session.h"
#include "theme.h"
#include "workspace.h"
#include "browser_tab.h"
#include <json-glib/json-glib.h>
#include <string.h>

static char *session_path(void) {
    char *dir = g_build_filename(g_get_home_dir(), ".prettymux", "sessions", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "last.json", NULL);
    g_free(dir);
    return path;
}

gboolean session_exists(void) {
    char *path = session_path();
    gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return exists;
}

void session_save(GtkWindow *window, GtkWidget *browser_notebook,
                  GtkWidget *terminal_stack, GtkWidget *workspace_list)
{
    (void)terminal_stack;
    (void)workspace_list;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "version");
    json_builder_add_int_value(b, 1);

    // Window size
    int w, h;
    gtk_window_get_default_size(window, &w, &h);
    json_builder_set_member_name(b, "windowW");
    json_builder_add_int_value(b, w > 0 ? w : 1400);
    json_builder_set_member_name(b, "windowH");
    json_builder_add_int_value(b, h > 0 ? h : 900);

    // Theme
    json_builder_set_member_name(b, "theme");
    json_builder_add_string_value(b, theme_get_current()->name);

    // Browser visible
    json_builder_set_member_name(b, "browserVisible");
    json_builder_add_boolean_value(b, gtk_widget_get_visible(browser_notebook));

    // Browser tabs
    json_builder_set_member_name(b, "browserTabs");
    json_builder_begin_array(b);
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser_notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(browser_notebook), i);
        if (BROWSER_IS_TAB(child)) {
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "url");
            const char *url = browser_tab_get_url(BROWSER_TAB(child));
            json_builder_add_string_value(b, url ? url : "about:blank");
            json_builder_end_object(b);
        }
    }
    json_builder_end_array(b);

    // Workspaces
    json_builder_set_member_name(b, "workspaces");
    json_builder_begin_array(b);
    if (workspaces) {
        for (guint i = 0; i < workspaces->len; i++) {
            Workspace *ws = g_ptr_array_index(workspaces, i);
            json_builder_begin_object(b);
            json_builder_set_member_name(b, "name");
            json_builder_add_string_value(b, ws->name);
            json_builder_set_member_name(b, "terminalCount");
            json_builder_add_int_value(b, ws->terminals->len);
            json_builder_end_object(b);
        }
    }
    json_builder_end_array(b);

    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, root);

    char *path = session_path();
    json_generator_to_file(gen, path, NULL);
    g_free(path);

    json_node_unref(root);
    g_object_unref(gen);
    g_object_unref(b);
}

typedef void (*AddBrowserTabFunc)(const char *url);
typedef void (*AddWorkspaceFunc)(GtkWidget *stack, GtkWidget *list, void *app);

void session_restore(GtkWindow *window, GtkWidget *browser_notebook,
                     GtkWidget *terminal_stack, GtkWidget *workspace_list,
                     void *ghostty_app)
{
    char *path = session_path();
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return;
    }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, NULL)) {
        g_object_unref(parser);
        g_free(path);
        return;
    }
    g_free(path);

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return;
    }

    JsonObject *obj = json_node_get_object(root);

    if (json_object_get_int_member_with_default(obj, "version", 0) != 1) {
        g_object_unref(parser);
        return;
    }

    // Window size
    int w = json_object_get_int_member_with_default(obj, "windowW", 1400);
    int h = json_object_get_int_member_with_default(obj, "windowH", 900);
    gtk_window_set_default_size(window, w, h);

    // Theme
    const char *theme_name = json_object_get_string_member_with_default(obj, "theme", "Dark");
    theme_set_by_name(theme_name);

    // Browser visibility
    gboolean browser_visible = json_object_get_boolean_member_with_default(obj, "browserVisible", TRUE);
    gtk_widget_set_visible(browser_notebook, browser_visible);

    // Workspace names (workspaces already created by caller)
    if (json_object_has_member(obj, "workspaces") && workspaces) {
        JsonArray *ws_arr = json_object_get_array_member(obj, "workspaces");
        guint len = json_array_get_length(ws_arr);

        // Create additional workspaces if needed
        for (guint i = workspaces->len; i < len; i++) {
            workspace_add(terminal_stack, workspace_list, ghostty_app);
        }

        // Set names
        for (guint i = 0; i < len && i < workspaces->len; i++) {
            JsonObject *ws_obj = json_array_get_object_element(ws_arr, i);
            Workspace *ws = g_ptr_array_index(workspaces, i);
            const char *name = json_object_get_string_member_with_default(ws_obj, "name", ws->name);
            snprintf(ws->name, sizeof(ws->name), "%s", name);

            GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(workspace_list), i);
            if (row) {
                GtkWidget *label = gtk_list_box_row_get_child(row);
                if (GTK_IS_LABEL(label))
                    gtk_label_set_text(GTK_LABEL(label), ws->name);
            }
        }
    }

    g_object_unref(parser);
    workspace_switch(0, terminal_stack, workspace_list);
}
