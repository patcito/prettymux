#include "session.h"
#include "theme.h"
#include "workspace.h"
#include "browser_tab.h"
#include "ghostty_terminal.h"
#include <json-glib/json-glib.h>
#include <string.h>

static gboolean session_restore_cwds_cb(gpointer data);

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

    /* Window size */
    int w, h;
    gtk_window_get_default_size(window, &w, &h);
    json_builder_set_member_name(b, "windowW");
    json_builder_add_int_value(b, w > 0 ? w : 1400);
    json_builder_set_member_name(b, "windowH");
    json_builder_add_int_value(b, h > 0 ? h : 900);

    /* Active workspace */
    json_builder_set_member_name(b, "activeWorkspace");
    json_builder_add_int_value(b, current_workspace);

    /* Theme */
    json_builder_set_member_name(b, "theme");
    json_builder_add_string_value(b, theme_get_current()->name);

    /* Browser visible */
    json_builder_set_member_name(b, "browserVisible");
    json_builder_add_boolean_value(b, gtk_widget_get_visible(browser_notebook));

    /* Browser tabs */
    json_builder_set_member_name(b, "browserTabs");
    json_builder_begin_array(b);
    {
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser_notebook));
        int i;
        for (i = 0; i < n; i++) {
            GtkWidget *child = gtk_notebook_get_nth_page(
                GTK_NOTEBOOK(browser_notebook), i);
            if (BROWSER_IS_TAB(child)) {
                json_builder_begin_object(b);
                json_builder_set_member_name(b, "url");
                const char *url = browser_tab_get_url(BROWSER_TAB(child));
                json_builder_add_string_value(b, url ? url : "about:blank");
                json_builder_set_member_name(b, "title");
                const char *title = browser_tab_get_title(BROWSER_TAB(child));
                json_builder_add_string_value(b, title ? title : "");
                json_builder_end_object(b);
            }
        }
    }
    json_builder_end_array(b);

    /* URL history */
    json_builder_set_member_name(b, "urlHistory");
    json_builder_begin_array(b);
    {
        GPtrArray *history = browser_tab_get_url_history();
        if (history) {
            guint i;
            for (i = 0; i < history->len; i++) {
                const char *entry = g_ptr_array_index(history, i);
                json_builder_add_string_value(b, entry);
            }
        }
    }
    json_builder_end_array(b);

    /* Workspaces with full pane structure */
    json_builder_set_member_name(b, "workspaces");
    json_builder_begin_array(b);
    if (workspaces) {
        guint wi;
        for (wi = 0; wi < workspaces->len; wi++) {
            Workspace *ws = g_ptr_array_index(workspaces, wi);
            json_builder_begin_object(b);

            json_builder_set_member_name(b, "name");
            json_builder_add_string_value(b, ws->name);

            json_builder_set_member_name(b, "notes");
            json_builder_add_string_value(b,
                ws->notes_text ? ws->notes_text : "");

            /* Panes array: one entry per pane notebook */
            json_builder_set_member_name(b, "panes");
            json_builder_begin_array(b);
            if (ws->pane_notebooks) {
                guint pi;
                for (pi = 0; pi < ws->pane_notebooks->len; pi++) {
                    GtkNotebook *nb = g_ptr_array_index(
                        ws->pane_notebooks, pi);
                    json_builder_begin_object(b);

                    /* Active tab in this pane */
                    json_builder_set_member_name(b, "activeTab");
                    json_builder_add_int_value(b,
                        gtk_notebook_get_current_page(nb));

                    /* Tabs array */
                    json_builder_set_member_name(b, "tabs");
                    json_builder_begin_array(b);
                    {
                        int n_pages = gtk_notebook_get_n_pages(nb);
                        int ti;
                        for (ti = 0; ti < n_pages; ti++) {
                            GtkWidget *child = gtk_notebook_get_nth_page(
                                nb, ti);
                            json_builder_begin_object(b);

                            /* Tab name */
                            GtkWidget *tab_widget =
                                gtk_notebook_get_tab_label(nb, child);
                            const char *tab_name = "Terminal";
                            if (tab_widget) {
                                GtkWidget *inner =
                                    gtk_widget_get_first_child(tab_widget);
                                if (GTK_IS_LABEL(inner))
                                    tab_name = gtk_label_get_text(
                                        GTK_LABEL(inner));
                            }
                            json_builder_set_member_name(b, "name");
                            json_builder_add_string_value(b, tab_name);

                            /* CWD */
                            const char *cwd = NULL;
                            if (GHOSTTY_IS_TERMINAL(child))
                                cwd = ghostty_terminal_get_cwd(
                                    GHOSTTY_TERMINAL(child));
                            json_builder_set_member_name(b, "cwd");
                            json_builder_add_string_value(b,
                                cwd ? cwd : "");

                            json_builder_end_object(b);
                        }
                    }
                    json_builder_end_array(b);

                    json_builder_end_object(b);
                }
            }
            json_builder_end_array(b);

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

void session_restore(GtkWindow *window, GtkWidget *browser_notebook,
                     GtkWidget *terminal_stack, GtkWidget *workspace_list,
                     ghostty_app_t ghostty_app,
                     SessionAddBrowserTabFunc add_browser_tab_func)
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

    /* Window size */
    int w = (int)json_object_get_int_member_with_default(obj, "windowW", 1400);
    int h = (int)json_object_get_int_member_with_default(obj, "windowH", 900);
    gtk_window_set_default_size(window, w, h);

    /* Theme */
    const char *theme_name = json_object_get_string_member_with_default(
        obj, "theme", "Dark");
    theme_set_by_name(theme_name);

    /* Browser visibility */
    gboolean browser_visible = json_object_get_boolean_member_with_default(
        obj, "browserVisible", TRUE);
    gtk_widget_set_visible(browser_notebook, browser_visible);

    /* URL history */
    if (json_object_has_member(obj, "urlHistory")) {
        JsonArray *url_arr = json_object_get_array_member(obj, "urlHistory");
        guint url_len = json_array_get_length(url_arr);
        GPtrArray *history = g_ptr_array_new_with_free_func(g_free);
        guint ui;
        for (ui = 0; ui < url_len; ui++) {
            const char *entry = json_array_get_string_element(url_arr, ui);
            if (entry && entry[0])
                g_ptr_array_add(history, g_strdup(entry));
        }
        browser_tab_set_url_history(history);
    }

    /* Restore browser tabs */
    if (json_object_has_member(obj, "browserTabs") && add_browser_tab_func) {
        JsonArray *bt_arr = json_object_get_array_member(obj, "browserTabs");
        guint bt_len = json_array_get_length(bt_arr);
        guint bi;
        for (bi = 0; bi < bt_len; bi++) {
            JsonObject *bt_obj = json_array_get_object_element(bt_arr, bi);
            const char *url = json_object_get_string_member_with_default(
                bt_obj, "url", "");
            if (url && url[0])
                add_browser_tab_func(url);
        }
    }

    /* Restore workspaces with full pane structure */
    if (json_object_has_member(obj, "workspaces") && workspaces) {
        JsonArray *ws_arr = json_object_get_array_member(obj, "workspaces");
        guint len = json_array_get_length(ws_arr);

        /* Create additional workspaces if needed
         * (the first one was already created by the caller) */
        guint wi;
        for (wi = workspaces->len; wi < len; wi++) {
            workspace_add(terminal_stack, workspace_list, ghostty_app);
        }

        /* Restore each workspace */
        for (wi = 0; wi < len && wi < workspaces->len; wi++) {
            JsonObject *ws_obj = json_array_get_object_element(ws_arr, wi);
            Workspace *ws = g_ptr_array_index(workspaces, wi);

            /* Restore name */
            const char *name = json_object_get_string_member_with_default(
                ws_obj, "name", ws->name);
            snprintf(ws->name, sizeof(ws->name), "%s", name);

            /* Restore notes */
            const char *notes = json_object_get_string_member_with_default(
                ws_obj, "notes", "");
            g_free(ws->notes_text);
            ws->notes_text = g_strdup(notes);

            /* Update sidebar label via the workspace's inner label */
            workspace_refresh_sidebar_label(ws);

            /* Restore panes.  The first pane (with one terminal)
             * was already created by workspace_add. */
            if (json_object_has_member(ws_obj, "panes")) {
                JsonArray *panes_arr = json_object_get_array_member(
                    ws_obj, "panes");
                guint n_panes = json_array_get_length(panes_arr);
                guint pi;

                for (pi = 0; pi < n_panes; pi++) {
                    JsonObject *pane_obj = json_array_get_object_element(
                        panes_arr, pi);

                    /* For panes beyond the first, split to create them */
                    if (pi > 0) {
                        workspace_split_pane(ws,
                            GTK_ORIENTATION_HORIZONTAL, ghostty_app);
                    }

                    /* Find the notebook for this pane */
                    GtkNotebook *nb = NULL;
                    if (pi < ws->pane_notebooks->len)
                        nb = g_ptr_array_index(ws->pane_notebooks, pi);
                    if (!nb)
                        continue;

                    /* Restore tabs.  The first tab in each pane
                     * was already created by the split or workspace_add. */
                    if (json_object_has_member(pane_obj, "tabs")) {
                        JsonArray *tabs_arr = json_object_get_array_member(
                            pane_obj, "tabs");
                        guint n_tabs = json_array_get_length(tabs_arr);
                        guint ti;

                        /* Create additional tabs */
                        for (ti = 1; ti < n_tabs; ti++) {
                            workspace_add_terminal_to_notebook_external(
                                ws, nb, ghostty_app);
                        }

                        /* Set tab names + restore CWD */
                        for (ti = 0; ti < n_tabs; ti++) {
                            JsonObject *tab_obj =
                                json_array_get_object_element(tabs_arr, ti);
                            const char *tab_name =
                                json_object_get_string_member_with_default(
                                    tab_obj, "name", "Terminal");
                            const char *saved_cwd =
                                json_object_get_string_member_with_default(
                                    tab_obj, "cwd", "");

                            int page_idx = (int)ti;
                            if (page_idx < gtk_notebook_get_n_pages(nb)) {
                                GtkWidget *child =
                                    gtk_notebook_get_nth_page(nb, page_idx);

                                /* Set tab name */
                                GtkWidget *tab_w =
                                    gtk_notebook_get_tab_label(nb, child);
                                if (tab_w) {
                                    GtkWidget *inner =
                                        gtk_widget_get_first_child(tab_w);
                                    if (GTK_IS_LABEL(inner))
                                        gtk_label_set_text(
                                            GTK_LABEL(inner), tab_name);
                                }

                                /* Restore CWD: type cd command after delay */
                                if (saved_cwd[0] && GHOSTTY_IS_TERMINAL(child)) {
                                    char *cmd = g_strdup_printf(
                                        "cd '%s' && clear\n",
                                        saved_cwd);
                                    /* Store cmd on the widget, type it after 800ms */
                                    g_object_set_data_full(G_OBJECT(child),
                                        "restore-cwd", cmd, g_free);
                                }
                            }
                        }

                        /* Restore active tab in this pane */
                        int active_tab = (int)
                            json_object_get_int_member_with_default(
                                pane_obj, "activeTab", 0);
                        if (active_tab >= 0 &&
                            active_tab < gtk_notebook_get_n_pages(nb))
                            gtk_notebook_set_current_page(nb, active_tab);
                    }
                }
            }
        }
    }

    /* Restore active workspace */
    int aw = (int)json_object_get_int_member_with_default(
        obj, "activeWorkspace", 0);
    if (aw >= 0 && workspaces && aw < (int)workspaces->len)
        workspace_switch(aw, terminal_stack, workspace_list);

    g_object_unref(parser);

    /* After 800ms, type 'cd <path> && clear' into terminals that had a saved CWD */
    g_timeout_add(800, session_restore_cwds_cb, NULL);
}

static gboolean
session_restore_cwds_cb(gpointer data)
{
    (void)data;
    if (!workspaces) return G_SOURCE_REMOVE;

    for (guint wi = 0; wi < workspaces->len; wi++) {
        Workspace *ws = g_ptr_array_index(workspaces, wi);
        if (!ws || !ws->pane_notebooks) continue;

        for (guint pi = 0; pi < ws->pane_notebooks->len; pi++) {
            GtkNotebook *nb = g_ptr_array_index(ws->pane_notebooks, pi);
            if (!GTK_IS_NOTEBOOK(nb)) continue;

            int n = gtk_notebook_get_n_pages(nb);
            for (int ti = 0; ti < n; ti++) {
                GtkWidget *child = gtk_notebook_get_nth_page(nb, ti);
                if (!child || !GHOSTTY_IS_TERMINAL(child)) continue;

                const char *cmd = g_object_get_data(G_OBJECT(child), "restore-cwd");
                if (!cmd || !cmd[0]) continue;

                ghostty_surface_t surface =
                    ghostty_terminal_get_surface(GHOSTTY_TERMINAL(child));
                if (surface) {
                    /* Use ghostty_surface_key with text to type the command */
                    ghostty_input_key_s ke = {0};
                    ke.action = GHOSTTY_ACTION_PRESS;
                    ke.keycode = 0;
                    ke.text = cmd;
                    ghostty_surface_key(surface, ke);
                }
                /* Clear the restore data */
                g_object_set_data(G_OBJECT(child), "restore-cwd", NULL);
            }
        }
    }
    return G_SOURCE_REMOVE;
}
