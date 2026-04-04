#include "session.h"
#include "theme.h"
#include "workspace.h"
#include "browser_tab.h"
#include "ghostty_terminal.h"
#include "project_icon_cache.h"
#include <json-glib/json-glib.h>
#include <string.h>

static GtkWindow *session_window = NULL;
static GtkWidget *session_browser_notebook = NULL;
static GtkWidget *session_terminal_stack = NULL;
static GtkWidget *session_workspace_list = NULL;
static guint session_save_source_id = 0;
static gboolean session_shutting_down = FALSE;

static GtkWidget *
session_page_linked_terminal(GtkWidget *page)
{
    GtkWidget *terminal;

    if (!page)
        return NULL;
    if (GHOSTTY_IS_TERMINAL(page))
        return page;

    terminal = g_object_get_data(G_OBJECT(page), "linked-terminal");
    return (terminal && GHOSTTY_IS_TERMINAL(terminal)) ? terminal : NULL;
}

typedef struct {
    JsonBuilder *builder;
} SessionLogoCacheSaveCtx;

static void
session_save_logo_cache_entry(const char *root,
                              const char *icon_path,
                              gpointer user_data)
{
    SessionLogoCacheSaveCtx *ctx = user_data;

    json_builder_begin_object(ctx->builder);
    json_builder_set_member_name(ctx->builder, "root");
    json_builder_add_string_value(ctx->builder, root ? root : "");
    json_builder_set_member_name(ctx->builder, "icon");
    json_builder_add_string_value(ctx->builder, icon_path ? icon_path : "");
    json_builder_end_object(ctx->builder);
}

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

static gboolean session_save_idle_cb(gpointer data) {
    (void)data;
    session_save_source_id = 0;

    if (session_window && session_browser_notebook &&
        session_terminal_stack && session_workspace_list) {
        session_save(session_window, session_browser_notebook,
                     session_terminal_stack, session_workspace_list);
    }

    return G_SOURCE_REMOVE;
}

typedef struct {
    GtkPaned *outer_paned;
    int outer_position;
    GtkPaned *main_paned;
    int main_position;
} SessionPanedRestoreData;

typedef struct {
    GtkPaned *paned;
    double ratio;
    guint attempts_left;
} SessionSplitRestoreData;

static GtkPaned *
session_main_paned(GtkWidget *browser_notebook)
{
    GtkWidget *parent = browser_notebook ? gtk_widget_get_parent(browser_notebook)
                                         : NULL;

    return GTK_IS_PANED(parent) ? GTK_PANED(parent) : NULL;
}

static GtkPaned *
session_outer_paned(GtkWidget *browser_notebook)
{
    GtkPaned *main_paned = session_main_paned(browser_notebook);
    GtkWidget *parent = main_paned ? gtk_widget_get_parent(GTK_WIDGET(main_paned))
                                   : NULL;

    return GTK_IS_PANED(parent) ? GTK_PANED(parent) : NULL;
}

static gboolean
session_restore_paned_positions_idle_cb(gpointer data)
{
    SessionPanedRestoreData *restore = data;

    if (restore->outer_paned && GTK_IS_PANED(restore->outer_paned))
        gtk_paned_set_position(restore->outer_paned, restore->outer_position);
    if (restore->main_paned && GTK_IS_PANED(restore->main_paned))
        gtk_paned_set_position(restore->main_paned, restore->main_position);

    if (restore->outer_paned)
        g_object_unref(restore->outer_paned);
    if (restore->main_paned)
        g_object_unref(restore->main_paned);
    g_free(restore);
    return G_SOURCE_REMOVE;
}

static double
session_paned_ratio(GtkPaned *paned)
{
    GtkOrientation orientation;
    int size;
    int position;

    if (!GTK_IS_PANED(paned))
        return 0.5;

    orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(paned));
    size = (orientation == GTK_ORIENTATION_HORIZONTAL)
        ? gtk_widget_get_width(GTK_WIDGET(paned))
        : gtk_widget_get_height(GTK_WIDGET(paned));
    position = gtk_paned_get_position(paned);

    if (size <= 1)
        return 0.5;

    if (position < 0)
        position = 0;
    if (position > size)
        position = size;

    return (double)position / (double)size;
}

static void
session_save_workspace_layout(JsonBuilder *builder,
                              Workspace *ws,
                              GtkWidget *widget)
{
    (void)ws;

    if (!widget) {
        json_builder_add_null_value(builder);
        return;
    }

    if (GTK_IS_NOTEBOOK(widget)) {
        const char *pane_id = workspace_get_pane_id(GTK_NOTEBOOK(widget));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "pane");
        json_builder_set_member_name(builder, "paneId");
        json_builder_add_string_value(builder, pane_id ? pane_id : "");
        json_builder_end_object(builder);
        return;
    }

    if (GTK_IS_PANED(widget)) {
        GtkPaned *paned = GTK_PANED(widget);
        GtkOrientation orientation =
            gtk_orientable_get_orientation(GTK_ORIENTABLE(widget));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "split");
        json_builder_set_member_name(builder, "orientation");
        json_builder_add_string_value(builder,
            orientation == GTK_ORIENTATION_HORIZONTAL
                ? "horizontal" : "vertical");
        json_builder_set_member_name(builder, "ratio");
        json_builder_add_double_value(builder, session_paned_ratio(paned));
        json_builder_set_member_name(builder, "start");
        session_save_workspace_layout(builder, ws,
            gtk_paned_get_start_child(paned));
        json_builder_set_member_name(builder, "end");
        session_save_workspace_layout(builder, ws,
            gtk_paned_get_end_child(paned));
        json_builder_end_object(builder);
        return;
    }

    json_builder_add_null_value(builder);
}

static gboolean
session_restore_split_position_cb(gpointer data)
{
    SessionSplitRestoreData *restore = data;
    GtkOrientation orientation;
    int size;
    int position;

    if (!restore || !GTK_IS_PANED(restore->paned)) {
        g_free(restore);
        return G_SOURCE_REMOVE;
    }

    orientation = gtk_orientable_get_orientation(
        GTK_ORIENTABLE(restore->paned));
    size = (orientation == GTK_ORIENTATION_HORIZONTAL)
        ? gtk_widget_get_width(GTK_WIDGET(restore->paned))
        : gtk_widget_get_height(GTK_WIDGET(restore->paned));

    if (size > 1) {
        position = (int)(restore->ratio * (double)size);
        if (position < 0)
            position = 0;
        if (position > size)
            position = size;
        gtk_paned_set_position(restore->paned, position);
    }

    if (restore->attempts_left > 0) {
        restore->attempts_left--;
        return G_SOURCE_CONTINUE;
    }

    g_object_unref(restore->paned);
    g_free(restore);
    return G_SOURCE_REMOVE;
}

static void
session_schedule_split_restore(GtkPaned *paned, double ratio)
{
    SessionSplitRestoreData *restore;

    if (!GTK_IS_PANED(paned))
        return;

    if (ratio < 0.0)
        ratio = 0.0;
    if (ratio > 1.0)
        ratio = 1.0;

    restore = g_new0(SessionSplitRestoreData, 1);
    restore->paned = g_object_ref(paned);
    restore->ratio = ratio;
    restore->attempts_left = 60;
    g_timeout_add(16, session_restore_split_position_cb, restore);
}

static void
session_assign_pane_id(GtkNotebook *pane, const char *pane_id)
{
    if (!GTK_IS_NOTEBOOK(pane) || !pane_id || !pane_id[0])
        return;

    g_object_set_data_full(G_OBJECT(pane), "pane-id", g_strdup(pane_id), g_free);
}

static gboolean
session_restore_workspace_layout_node(Workspace *ws,
                                      JsonObject *layout_obj,
                                      GtkNotebook *seed_pane,
                                      ghostty_app_t ghostty_app)
{
    const char *type;

    if (!ws || !layout_obj || !GTK_IS_NOTEBOOK(seed_pane))
        return FALSE;

    type = json_object_get_string_member_with_default(layout_obj, "type", "");
    if (g_strcmp0(type, "pane") == 0) {
        const char *pane_id = json_object_get_string_member_with_default(
            layout_obj, "paneId", "");

        session_assign_pane_id(seed_pane, pane_id);
        return TRUE;
    }

    if (g_strcmp0(type, "split") == 0) {
        const char *orientation_name =
            json_object_get_string_member_with_default(layout_obj,
                                                       "orientation",
                                                       "horizontal");
        GtkOrientation orientation =
            g_strcmp0(orientation_name, "vertical") == 0
                ? GTK_ORIENTATION_VERTICAL
                : GTK_ORIENTATION_HORIZONTAL;
        JsonObject *start_obj = json_object_get_object_member(layout_obj, "start");
        JsonObject *end_obj = json_object_get_object_member(layout_obj, "end");
        GtkNotebook *new_pane;
        GtkWidget *parent;
        double ratio = json_object_get_double_member_with_default(layout_obj,
                                                                  "ratio",
                                                                  0.5);

        if (!start_obj || !end_obj)
            return FALSE;

        new_pane = workspace_split_pane_target(ws, seed_pane, orientation,
                                               ghostty_app);
        if (!GTK_IS_NOTEBOOK(new_pane))
            return FALSE;

        if (!session_restore_workspace_layout_node(ws, start_obj, seed_pane,
                                                   ghostty_app))
            return FALSE;
        if (!session_restore_workspace_layout_node(ws, end_obj, new_pane,
                                                   ghostty_app))
            return FALSE;

        parent = gtk_widget_get_parent(GTK_WIDGET(seed_pane));
        if (GTK_IS_PANED(parent))
            session_schedule_split_restore(GTK_PANED(parent), ratio);

        return TRUE;
    }

    return FALSE;
}

void session_set_context(GtkWindow *window, GtkWidget *browser_notebook,
                         GtkWidget *terminal_stack, GtkWidget *workspace_list)
{
    session_window = window;
    session_browser_notebook = browser_notebook;
    session_terminal_stack = terminal_stack;
    session_workspace_list = workspace_list;
    session_shutting_down = FALSE;
}

void session_begin_shutdown(void)
{
    session_shutting_down = TRUE;
    if (session_save_source_id != 0) {
        g_source_remove(session_save_source_id);
        session_save_source_id = 0;
    }
}

void session_queue_save(void)
{
    if (session_shutting_down)
        return;

    if (!session_window || !session_browser_notebook ||
        !session_terminal_stack || !session_workspace_list)
        return;

    if (session_save_source_id != 0)
        return;

    session_save_source_id = g_idle_add(session_save_idle_cb, NULL);
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

    GtkPaned *outer_paned = session_outer_paned(browser_notebook);
    GtkPaned *main_paned = session_main_paned(browser_notebook);

    json_builder_set_member_name(b, "outerPanedPos");
    json_builder_add_int_value(b,
        outer_paned ? gtk_paned_get_position(outer_paned) : 200);
    json_builder_set_member_name(b, "mainPanedPos");
    json_builder_add_int_value(b,
        main_paned ? gtk_paned_get_position(main_paned) : 700);

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

    /* Project icon cache */
    json_builder_set_member_name(b, "logoCache");
    json_builder_begin_array(b);
    {
        SessionLogoCacheSaveCtx ctx = { .builder = b };
        project_icon_cache_foreach(session_save_logo_cache_entry, &ctx);
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

            json_builder_set_member_name(b, "layout");
            session_save_workspace_layout(
                b, ws, gtk_overlay_get_child(GTK_OVERLAY(ws->overlay)));

            /* Panes array: one entry per pane notebook */
            json_builder_set_member_name(b, "panes");
            json_builder_begin_array(b);
            if (ws->pane_notebooks) {
                guint pi;
                for (pi = 0; pi < ws->pane_notebooks->len; pi++) {
                    GtkNotebook *nb = g_ptr_array_index(
                        ws->pane_notebooks, pi);
                    const char *pane_id = workspace_get_pane_id(nb);
                    json_builder_begin_object(b);

                    json_builder_set_member_name(b, "paneId");
                    json_builder_add_string_value(b, pane_id ? pane_id : "");

                    /* Active tab in this pane */
                    json_builder_set_member_name(b, "activeTab");
                    int n_pages = GTK_IS_NOTEBOOK(nb) ? gtk_notebook_get_n_pages(nb) : 0;
                    json_builder_add_int_value(b,
                        GTK_IS_NOTEBOOK(nb) ? gtk_notebook_get_current_page(nb) : 0);

                    /* Tabs array */
                    json_builder_set_member_name(b, "tabs");
                    json_builder_begin_array(b);
                    {
                        int ti;
                        for (ti = 0; ti < n_pages; ti++) {
                            GtkWidget *child = gtk_notebook_get_nth_page(
                                nb, ti);
                            GtkWidget *terminal =
                                session_page_linked_terminal(child);
                            json_builder_begin_object(b);

                            /* Tab name */
                            GtkWidget *tab_widget =
                                gtk_notebook_get_tab_label(nb, child);
                            /* Find the GtkLabel inside the tab widget box */
                            const char *tab_name = "Terminal";
                            gboolean is_custom = FALSE;
                            if (tab_widget) {
                                for (GtkWidget *w = gtk_widget_get_first_child(tab_widget);
                                     w; w = gtk_widget_get_next_sibling(w)) {
                                    if (GTK_IS_LABEL(w)) {
                                        tab_name = gtk_label_get_text(GTK_LABEL(w));
                                        if (g_object_get_data(G_OBJECT(w), "user-renamed"))
                                            is_custom = TRUE;
                                        break;
                                    }
                                }
                            }
                            json_builder_set_member_name(b, "name");
                            json_builder_add_string_value(b,
                                tab_name ? tab_name : "Terminal");
                            json_builder_set_member_name(b, "customName");
                            json_builder_add_boolean_value(b, is_custom);

                            /* CWD */
                            const char *cwd = NULL;
                            if (GHOSTTY_IS_TERMINAL(terminal))
                                cwd = ghostty_terminal_get_cwd(
                                    GHOSTTY_TERMINAL(terminal));
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

    int outer_paned_pos = (int)json_object_get_int_member_with_default(
        obj, "outerPanedPos", 200);
    int main_paned_pos = (int)json_object_get_int_member_with_default(
        obj, "mainPanedPos", 700);

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

    if (json_object_has_member(obj, "logoCache")) {
        JsonArray *logo_arr = json_object_get_array_member(obj, "logoCache");
        guint logo_len = json_array_get_length(logo_arr);

        for (guint li = 0; li < logo_len; li++) {
            JsonNode *logo_node = json_array_get_element(logo_arr, li);
            JsonObject *logo_obj;
            const char *root_path;
            const char *icon_path;

            if (!logo_node || !JSON_NODE_HOLDS_OBJECT(logo_node))
                continue;

            logo_obj = json_node_get_object(logo_node);
            root_path = json_object_get_string_member_with_default(
                logo_obj, "root", "");
            icon_path = json_object_get_string_member_with_default(
                logo_obj, "icon", "");
            if (root_path[0] && icon_path[0])
                project_icon_cache_restore_entry(root_path, icon_path);
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
                gboolean restored_layout = FALSE;

                if (json_object_has_member(ws_obj, "layout")) {
                    JsonObject *layout_obj = json_object_get_object_member(
                        ws_obj, "layout");
                    if (layout_obj) {
                        restored_layout = session_restore_workspace_layout_node(
                            ws, layout_obj, GTK_NOTEBOOK(ws->notebook),
                            ghostty_app);
                    }
                }

                JsonArray *panes_arr = json_object_get_array_member(
                    ws_obj, "panes");
                guint n_panes = json_array_get_length(panes_arr);
                guint pi;

                for (pi = 0; pi < n_panes; pi++) {
                    JsonNode *pane_node = json_array_get_element(
                        panes_arr, pi);
                    const char *saved_pane_id;
                    if (!pane_node || !JSON_NODE_HOLDS_OBJECT(pane_node))
                        continue;
                    JsonObject *pane_obj = json_node_get_object(pane_node);
                    GtkNotebook *nb = NULL;

                    saved_pane_id =
                        json_object_get_string_member_with_default(
                            pane_obj, "paneId", "");

                    if (saved_pane_id[0])
                        nb = workspace_get_pane_by_id(ws, saved_pane_id);
                    else if (!restored_layout && pi > 0) {
                        workspace_split_pane(ws,
                            GTK_ORIENTATION_HORIZONTAL, ghostty_app);
                    }

                    if (!nb && pi < ws->pane_notebooks->len)
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
                                    GtkWidget *old_terminal =
                                        session_page_linked_terminal(old);
                                    if (old_terminal) {
                                        g_ptr_array_remove(ws->terminals,
                                                           old_terminal);
                                        if (ws->overlay) {
                                            gtk_overlay_remove_overlay(
                                                GTK_OVERLAY(ws->overlay),
                                                old_terminal);
                                        }
                                    }
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
                                    /* Find the GtkLabel in the tab box */
                                    for (GtkWidget *w = gtk_widget_get_first_child(tab_w);
                                         w; w = gtk_widget_get_next_sibling(w)) {
                                        if (GTK_IS_LABEL(w)) {
                                            gtk_label_set_text(GTK_LABEL(w), tab_name);
                                            if (is_custom)
                                                g_object_set_data(
                                                    G_OBJECT(w), "user-renamed",
                                                    GINT_TO_POINTER(1));
                                            break;
                                        }
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

    {
        SessionPanedRestoreData *restore = g_new0(SessionPanedRestoreData, 1);
        restore->outer_paned = session_outer_paned(browser_notebook);
        restore->outer_position = outer_paned_pos;
        restore->main_paned = session_main_paned(browser_notebook);
        restore->main_position = main_paned_pos;

        if (restore->outer_paned)
            g_object_ref(restore->outer_paned);
        if (restore->main_paned)
            g_object_ref(restore->main_paned);

        g_idle_add(session_restore_paned_positions_idle_cb, restore);
    }

    g_object_unref(parser);

    /* CWD is now set via ghostty_terminal_new(cwd) — no cd hack needed */
}
