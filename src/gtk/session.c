#include "session.h"
#include "theme.h"
#include "workspace.h"
#include "browser_tab.h"
#include "ghostty_terminal.h"
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

                            /* Save whether this was a user-set custom name */
                            gboolean is_custom = FALSE;
                            if (tab_widget) {
                                GtkWidget *inner2 =
                                    gtk_widget_get_first_child(tab_widget);
                                if (inner2 && g_object_get_data(
                                        G_OBJECT(inner2), "user-renamed"))
                                    is_custom = TRUE;
                            }
                            json_builder_set_member_name(b, "customName");
                            json_builder_add_boolean_value(b, is_custom);

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
            JsonNode *bt_node = json_array_get_element(bt_arr, bi);
            if (!bt_node || !JSON_NODE_HOLDS_OBJECT(bt_node))
                continue;
            JsonObject *bt_obj = json_node_get_object(bt_node);
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
            JsonNode *ws_node = json_array_get_element(ws_arr, wi);
            if (!ws_node || !JSON_NODE_HOLDS_OBJECT(ws_node))
                continue;
            JsonObject *ws_obj = json_node_get_object(ws_node);
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
                    JsonNode *pane_node = json_array_get_element(
                        panes_arr, pi);
                    if (!pane_node || !JSON_NODE_HOLDS_OBJECT(pane_node))
                        continue;
                    JsonObject *pane_obj = json_node_get_object(pane_node);

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

                        /* Replace the auto-created first tab with one
                         * that has the correct CWD, then create the rest. */
                        if (n_tabs > 0) {
                            JsonNode *first_node =
                                json_array_get_element(tabs_arr, 0);
                            const char *first_cwd = "";
                            if (first_node && JSON_NODE_HOLDS_OBJECT(first_node)) {
                                JsonObject *fo = json_node_get_object(first_node);
                                first_cwd =
                                    json_object_get_string_member_with_default(
                                        fo, "cwd", "");
                            }
                            if (first_cwd[0]) {
                                /* Remove the placeholder tab and create
                                 * a new one with the correct CWD */
                                int n_existing = gtk_notebook_get_n_pages(nb);
                                if (n_existing > 0) {
                                    GtkWidget *old = gtk_notebook_get_nth_page(nb, 0);
                                    g_ptr_array_remove(ws->terminals, old);
                                    gtk_notebook_remove_page(nb, 0);
                                }
                                workspace_add_terminal_to_notebook_with_cwd(
                                    ws, nb, ghostty_app, first_cwd);
                            }
                        }

                        /* Create additional tabs (ti=1+) with saved CWD */
                        for (ti = 1; ti < n_tabs; ti++) {
                            JsonNode *tab_node =
                                json_array_get_element(tabs_arr, ti);
                            const char *saved_cwd = "";
                            if (tab_node && JSON_NODE_HOLDS_OBJECT(tab_node)) {
                                JsonObject *tab_obj =
                                    json_node_get_object(tab_node);
                                saved_cwd =
                                    json_object_get_string_member_with_default(
                                        tab_obj, "cwd", "");
                            }
                            workspace_add_terminal_to_notebook_with_cwd(
                                ws, nb, ghostty_app,
                                saved_cwd[0] ? saved_cwd : NULL);
                        }

                        /* Set tab names (CWD already handled above) */
                        for (ti = 0; ti < n_tabs; ti++) {
                            JsonNode *tab_node =
                                json_array_get_element(tabs_arr, ti);
                            if (!tab_node || !JSON_NODE_HOLDS_OBJECT(tab_node))
                                continue;
                            JsonObject *tab_obj =
                                json_node_get_object(tab_node);
                            const char *tab_name =
                                json_object_get_string_member_with_default(
                                    tab_obj, "name", "Terminal");

                            int page_idx = (int)ti;
                            if (page_idx < gtk_notebook_get_n_pages(nb)) {
                                GtkWidget *child =
                                    gtk_notebook_get_nth_page(nb, page_idx);
                                GtkWidget *tab_w =
                                    gtk_notebook_get_tab_label(nb, child);
                                gboolean is_custom =
                                    json_object_get_boolean_member_with_default(
                                        tab_obj, "customName", FALSE);
                                if (tab_w) {
                                    GtkWidget *inner =
                                        gtk_widget_get_first_child(tab_w);
                                    if (GTK_IS_LABEL(inner)) {
                                        gtk_label_set_text(
                                            GTK_LABEL(inner), tab_name);
                                        /* Only mark as user-renamed if
                                         * the user explicitly set it */
                                        if (is_custom)
                                            g_object_set_data(
                                                G_OBJECT(inner),
                                                "user-renamed",
                                                GINT_TO_POINTER(1));
                                    }
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

    /* CWD is now set via ghostty_terminal_new(cwd) — no cd hack needed */
}

