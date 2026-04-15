#include "session.h"
#include "browser_tab.h"
#include "ghostty_terminal.h"
#include "project_icon_cache.h"
#include "theme.h"
#include "workspace.h"
#include "workspace_strip.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

GPtrArray *workspaces;
int current_workspace;

static int strip_apply_layout_calls;
static int strip_focus_column_calls;
static guint pane_serial;

static const Theme test_theme = {
    .name = "Test Theme",
};

typedef struct {
    GtkWindow *window;
    GtkWidget *browser_notebook;
    GtkWidget *terminal_stack;
    GtkWidget *workspace_list;
} SessionTestUi;

static void test_workspace_free(Workspace *ws);

static char *
test_session_path_for_instance(const char *instance_id)
{
    return session_get_instance_session_path(instance_id);
}

static char *
test_legacy_session_path(void)
{
    return g_build_filename(g_get_home_dir(), ".prettymux", "sessions",
                            "last.json", NULL);
}

static void
test_clear_session_file_for_instance(const char *instance_id)
{
    char *path = test_session_path_for_instance(instance_id);
    g_remove(path);
    g_free(path);
}

static void
test_clear_legacy_session_file(void)
{
    char *path = test_legacy_session_path();
    g_remove(path);
    g_free(path);
}

static JsonObject *
test_load_saved_session_for_instance(const char *instance_id)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root;
    JsonObject *obj;
    char *path = test_session_path_for_instance(instance_id);

    g_assert_true(json_parser_load_from_file(parser, path, NULL));
    root = json_parser_get_root(parser);
    g_assert_nonnull(root);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
    obj = json_node_dup_object(root);

    g_object_unref(parser);
    g_free(path);
    return obj;
}

static Workspace *
test_first_workspace(void)
{
    if (!workspaces || workspaces->len == 0)
        return NULL;
    return g_ptr_array_index(workspaces, 0);
}

static SessionTestUi
test_ui_new(void)
{
    SessionTestUi ui = {0};
    GtkWidget *outer;
    GtkWidget *main_paned;

    ui.window = GTK_WINDOW(gtk_window_new());
    g_object_ref_sink(ui.window);
    ui.browser_notebook = gtk_notebook_new();
    ui.terminal_stack = gtk_stack_new();
    ui.workspace_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    main_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(main_paned), ui.browser_notebook);
    gtk_paned_set_end_child(GTK_PANED(main_paned), ui.terminal_stack);

    outer = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(outer), ui.workspace_list);
    gtk_paned_set_end_child(GTK_PANED(outer), main_paned);
    gtk_window_set_child(ui.window, outer);

    gtk_window_set_default_size(ui.window, 1280, 720);
    gtk_paned_set_position(GTK_PANED(outer), 220);
    gtk_paned_set_position(GTK_PANED(main_paned), 860);

    return ui;
}

static void
test_ui_free(SessionTestUi *ui)
{
    if (!ui || !ui->window)
        return;
    g_object_unref(ui->window);
}

static void
test_reset_workspaces(void)
{
    if (workspaces)
        g_ptr_array_unref(workspaces);

    workspaces = g_ptr_array_new_with_free_func((GDestroyNotify)test_workspace_free);
    current_workspace = 0;
    strip_apply_layout_calls = 0;
    strip_focus_column_calls = 0;
    pane_serial = 1;
}

static GtkWidget *
test_make_notebook_page(GtkNotebook *notebook, const char *title)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new(title ? title : "Terminal");

    gtk_box_append(GTK_BOX(tab_box), label);
    gtk_notebook_append_page(notebook, page, tab_box);
    return page;
}

static void
test_assign_pane_id(GtkNotebook *pane, const char *pane_id)
{
    g_object_set_data_full(G_OBJECT(pane), "pane-id", g_strdup(pane_id), g_free);
}

static GtkNotebook *
test_workspace_add_pane(Workspace *ws, int idx)
{
    GtkWidget *nb_widget = gtk_notebook_new();
    GtkNotebook *nb = GTK_NOTEBOOK(nb_widget);
    char *pane_id = g_strdup_printf("pane-%d", idx);

    test_assign_pane_id(nb, pane_id);
    g_free(pane_id);

    test_make_notebook_page(nb, "Terminal");
    g_ptr_array_add(ws->pane_notebooks, nb);

    return nb;
}

static Workspace *
test_workspace_new(WorkspaceLayoutMode mode, guint panes, const char *name)
{
    Workspace *ws = g_new0(Workspace, 1);

    ws->layout_mode = WORKSPACE_LAYOUT_CLASSIC;
    ws->overlay = gtk_overlay_new();
    g_object_ref_sink(ws->overlay);
    ws->container = ws->overlay;
    ws->pane_notebooks = g_ptr_array_new();
    ws->terminals = g_ptr_array_new();

    snprintf(ws->name, sizeof(ws->name), "%s", name ? name : "Workspace");

    if (panes == 0)
        panes = 1;

    for (guint i = 0; i < panes; i++) {
        GtkNotebook *pane = test_workspace_add_pane(ws, (int)i);
        if (i == 0) {
            ws->notebook = GTK_WIDGET(pane);
            ws->active_pane = pane;
            gtk_overlay_set_child(GTK_OVERLAY(ws->overlay), GTK_WIDGET(pane));
        }
    }

    if (mode == WORKSPACE_LAYOUT_STRIP)
        workspace_rebuild_for_layout_mode(ws, WORKSPACE_LAYOUT_STRIP);

    return ws;
}

static void
test_workspace_free(Workspace *ws)
{
    if (!ws)
        return;

    if (ws->strip_state)
        workspace_strip_state_free(ws->strip_state);
    if (ws->pane_notebooks)
        g_ptr_array_unref(ws->pane_notebooks);
    if (ws->terminals)
        g_ptr_array_unref(ws->terminals);
    if (ws->overlay)
        g_object_unref(ws->overlay);
    g_free(ws->notes_text);
    g_free(ws);
}

/* ---- app/theme/browser/icon stubs ---- */

const Theme *
theme_get_current(void)
{
    return &test_theme;
}

void
theme_set_by_name(const char *name)
{
    (void)name;
}

GType
browser_tab_get_type(void)
{
    return GTK_TYPE_BOX;
}

const char *
browser_tab_get_url(BrowserTab *self)
{
    (void)self;
    return "about:blank";
}

const char *
browser_tab_get_title(BrowserTab *self)
{
    (void)self;
    return "";
}

GPtrArray *
browser_tab_get_url_history(void)
{
    return NULL;
}

void
browser_tab_set_url_history(GPtrArray *history)
{
    if (history)
        g_ptr_array_unref(history);
}

void
project_icon_cache_foreach(ProjectIconCacheForeachFunc func, gpointer user_data)
{
    (void)func;
    (void)user_data;
}

void
project_icon_cache_restore_entry(const char *root, const char *icon_path)
{
    (void)root;
    (void)icon_path;
}

GType
ghostty_terminal_get_type(void)
{
    return G_TYPE_INVALID;
}

const char *
ghostty_terminal_get_cwd(GhosttyTerminal *self)
{
    (void)self;
    return NULL;
}

const char *
app_state_get_instance_id(void)
{
    const char *instance_id = g_getenv("PRETTYMUX_INSTANCE_ID");

    if (instance_id && instance_id[0])
        return instance_id;

    return "default";
}

/* ---- workspace/layout stubs used by session.c ---- */

WorkspaceLayoutMode
workspace_get_layout_mode(Workspace *ws)
{
    if (!ws)
        return WORKSPACE_LAYOUT_CLASSIC;
    return ws->layout_mode;
}

const char *
workspace_get_pane_id(GtkNotebook *pane)
{
    if (!pane)
        return "";
    return g_object_get_data(G_OBJECT(pane), "pane-id");
}

GtkNotebook *
workspace_get_pane_by_id(Workspace *ws, const char *pane_id)
{
    if (!ws || !ws->pane_notebooks || !pane_id)
        return NULL;

    for (guint i = 0; i < ws->pane_notebooks->len; i++) {
        GtkNotebook *pane = g_ptr_array_index(ws->pane_notebooks, i);
        if (g_strcmp0(workspace_get_pane_id(pane), pane_id) == 0)
            return pane;
    }

    return NULL;
}

int
workspace_get_pane_index(Workspace *ws, GtkNotebook *pane)
{
    if (!ws || !ws->pane_notebooks || !pane)
        return -1;

    for (guint i = 0; i < ws->pane_notebooks->len; i++) {
        if (g_ptr_array_index(ws->pane_notebooks, i) == pane)
            return (int)i;
    }

    return -1;
}

WorkspaceStripState *
workspace_strip_state_new(void)
{
    WorkspaceStripState *state = g_new0(WorkspaceStripState, 1);
    state->columns = g_ptr_array_new_with_free_func(g_free);
    state->focused_col = 0;
    return state;
}

void
workspace_strip_state_free(WorkspaceStripState *state)
{
    if (!state)
        return;
    if (state->columns)
        g_ptr_array_unref(state->columns);
    g_free(state);
}

static void
test_strip_rebuild_columns_from_panes(Workspace *ws)
{
    WorkspaceStripState *state;

    if (!ws)
        return;

    if (ws->strip_state)
        workspace_strip_state_free(ws->strip_state);
    ws->strip_state = workspace_strip_state_new();
    state = ws->strip_state;

    for (guint i = 0; ws->pane_notebooks && i < ws->pane_notebooks->len; i++) {
        WorkspaceColumn *col = g_new0(WorkspaceColumn, 1);
        col->notebook = g_ptr_array_index(ws->pane_notebooks, i);
        col->target_width = 600;
        col->current_width = 600.0;
        col->maximized = FALSE;
        g_ptr_array_add(state->columns, col);
    }

    state->focused_col = workspace_get_pane_index(ws, ws->active_pane);
    if (state->focused_col < 0)
        state->focused_col = 0;
}

gboolean
workspace_rebuild_for_layout_mode(Workspace *ws, WorkspaceLayoutMode mode)
{
    if (!ws)
        return FALSE;

    if (mode == WORKSPACE_LAYOUT_STRIP) {
        ws->layout_mode = WORKSPACE_LAYOUT_STRIP;
        test_strip_rebuild_columns_from_panes(ws);
        return TRUE;
    }

    ws->layout_mode = WORKSPACE_LAYOUT_CLASSIC;
    if (ws->strip_state) {
        workspace_strip_state_free(ws->strip_state);
        ws->strip_state = NULL;
    }
    return TRUE;
}

static GtkNotebook *
test_workspace_insert_pane_after(Workspace *ws, GtkNotebook *anchor)
{
    GtkWidget *nb_widget;
    GtkNotebook *nb;
    int insert_idx;

    if (!ws)
        return NULL;

    nb_widget = gtk_notebook_new();
    nb = GTK_NOTEBOOK(nb_widget);

    {
        char *pane_id = g_strdup_printf("pane-temp-%u", pane_serial++);
        test_assign_pane_id(nb, pane_id);
        g_free(pane_id);
    }

    test_make_notebook_page(nb, "Terminal");

    insert_idx = workspace_get_pane_index(ws, anchor);
    if (insert_idx < 0)
        insert_idx = (int)(ws->pane_notebooks ? ws->pane_notebooks->len : 0);
    else
        insert_idx++;

    if (!ws->pane_notebooks)
        ws->pane_notebooks = g_ptr_array_new();
    g_ptr_array_insert(ws->pane_notebooks, insert_idx, nb);

    ws->active_pane = nb;

    if (ws->strip_state && ws->strip_state->columns) {
        WorkspaceColumn *new_col = g_new0(WorkspaceColumn, 1);
        int focused = ws->strip_state->focused_col;
        int col_insert = insert_idx;

        new_col->notebook = GTK_WIDGET(nb);
        new_col->target_width = 600;
        new_col->current_width = 600.0;
        if (focused >= 0 && focused < (int)ws->strip_state->columns->len)
            col_insert = focused + 1;
        if (col_insert < 0)
            col_insert = 0;
        if (col_insert > (int)ws->strip_state->columns->len)
            col_insert = (int)ws->strip_state->columns->len;
        g_ptr_array_insert(ws->strip_state->columns, col_insert, new_col);
        ws->strip_state->focused_col = col_insert;
    }

    return nb;
}

gboolean
workspace_split_current_for_layout(Workspace *ws, GtkOrientation orientation, ghostty_app_t app)
{
    GtkNotebook *anchor;

    (void)app;
    if (!ws || orientation != GTK_ORIENTATION_HORIZONTAL)
        return FALSE;
    if (workspace_get_layout_mode(ws) != WORKSPACE_LAYOUT_STRIP)
        return FALSE;

    anchor = ws->active_pane ? ws->active_pane : GTK_NOTEBOOK(ws->notebook);
    return test_workspace_insert_pane_after(ws, anchor) != NULL;
}

GtkNotebook *
workspace_split_pane_target(Workspace *ws, GtkNotebook *pane, GtkOrientation orientation, ghostty_app_t app)
{
    (void)orientation;
    (void)app;
    return test_workspace_insert_pane_after(ws, pane);
}

void
workspace_split_pane(Workspace *ws, GtkOrientation orientation, ghostty_app_t app)
{
    (void)ws;
    (void)orientation;
    (void)app;
}

void
workspace_add_terminal_to_notebook_with_cwd(Workspace *ws,
                                             GtkNotebook *notebook,
                                             ghostty_app_t app,
                                             const char *cwd)
{
    (void)app;

    if (!ws || !GTK_IS_NOTEBOOK(notebook))
        return;

    test_make_notebook_page(notebook, "Terminal");

    if (cwd && cwd[0])
        snprintf(ws->cwd, sizeof(ws->cwd), "%s", cwd);
}

void
workspace_add(GtkWidget *terminal_stack, GtkWidget *workspace_list, ghostty_app_t app)
{
    Workspace *ws;

    (void)terminal_stack;
    (void)workspace_list;
    (void)app;

    if (!workspaces)
        workspaces = g_ptr_array_new_with_free_func((GDestroyNotify)test_workspace_free);

    ws = test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Workspace");
    g_ptr_array_add(workspaces, ws);
}

void
workspace_refresh_sidebar_label(Workspace *ws)
{
    (void)ws;
}

void
workspace_detect_git(Workspace *ws)
{
    (void)ws;
}

void
workspace_switch(int index, GtkWidget *terminal_stack, GtkWidget *workspace_list)
{
    (void)terminal_stack;
    (void)workspace_list;
    current_workspace = index;
}

void
workspace_strip_apply_layout(Workspace *ws)
{
    (void)ws;
    strip_apply_layout_calls++;
}

GtkNotebook *
workspace_strip_column_focused_notebook(WorkspaceColumn *col)
{
    if (!col)
        return NULL;
    if (col->panes && col->panes->len > 0) {
        int pane_idx = CLAMP(col->focused_pane, 0, (int)col->panes->len - 1);
        GtkWidget *pane_widget = g_ptr_array_index(col->panes, pane_idx);
        if (GTK_IS_NOTEBOOK(pane_widget))
            return GTK_NOTEBOOK(pane_widget);
    }
    if (GTK_IS_NOTEBOOK(col->notebook))
        return GTK_NOTEBOOK(col->notebook);
    return NULL;
}

void
workspace_strip_focus_column(Workspace *ws, int col_idx)
{
    strip_focus_column_calls++;
    if (ws && ws->strip_state)
        ws->strip_state->focused_col = col_idx;
}

/* ---- integration tests ---- */

static void
test_save_and_restore_strip_round_trip(void)
{
    SessionTestUi ui;
    Workspace *saved_ws;
    WorkspaceStripState *saved_state;
    JsonObject *saved_root;
    JsonArray *saved_workspaces;
    JsonObject *saved_ws_obj;
    JsonObject *saved_strip_obj;
    JsonArray *saved_columns;
    Workspace *restored_ws;
    WorkspaceStripState *restored_state;
    WorkspaceColumn *col0;
    WorkspaceColumn *col1;
    WorkspaceColumn *col2;

    test_reset_workspaces();
    test_clear_session_file_for_instance(NULL);
    ui = test_ui_new();

    saved_ws = test_workspace_new(WORKSPACE_LAYOUT_STRIP, 3, "Strip WS");
    saved_state = saved_ws->strip_state;
    g_ptr_array_add(workspaces, saved_ws);

    saved_state->focused_col = 2;
    ((WorkspaceColumn *)g_ptr_array_index(saved_state->columns, 0))->target_width = 480;
    ((WorkspaceColumn *)g_ptr_array_index(saved_state->columns, 0))->maximized = TRUE;
    ((WorkspaceColumn *)g_ptr_array_index(saved_state->columns, 1))->target_width = 720;
    ((WorkspaceColumn *)g_ptr_array_index(saved_state->columns, 2))->target_width = 960;
    ((WorkspaceColumn *)g_ptr_array_index(saved_state->columns, 2))->maximized = TRUE;

    session_save(ui.window, ui.browser_notebook, ui.terminal_stack, ui.workspace_list);

    saved_root = test_load_saved_session_for_instance(NULL);
    saved_workspaces = json_object_get_array_member(saved_root, "workspaces");
    saved_ws_obj = json_array_get_object_element(saved_workspaces, 0);
    saved_strip_obj = json_object_get_object_member(saved_ws_obj, "stripState");
    saved_columns = json_object_get_array_member(saved_strip_obj, "columns");

    g_assert_cmpstr(json_object_get_string_member(saved_ws_obj, "layoutMode"), ==, "strip");
    g_assert_cmpuint(json_array_get_length(saved_columns), ==, 3);
    g_assert_cmpint((int)json_object_get_int_member(
                        json_array_get_object_element(saved_columns, 0), "width"),
                    ==, 480);
    g_assert_true(json_object_get_boolean_member(
        json_array_get_object_element(saved_columns, 0), "maximized"));
    g_assert_cmpint((int)json_object_get_int_member(
                        json_array_get_object_element(saved_columns, 2), "width"),
                    ==, 960);
    g_assert_true(json_object_get_boolean_member(
        json_array_get_object_element(saved_columns, 2), "maximized"));
    g_assert_cmpint((int)json_object_get_int_member(saved_strip_obj, "focusedColumn"), ==, 2);

    json_object_unref(saved_root);

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Bootstrap"));

    session_restore(ui.window,
                    ui.browser_notebook,
                    ui.terminal_stack,
                    ui.workspace_list,
                    NULL,
                    NULL);

    g_assert_cmpuint(workspaces->len, ==, 1);
    restored_ws = g_ptr_array_index(workspaces, 0);
    g_assert_cmpint(restored_ws->layout_mode, ==, WORKSPACE_LAYOUT_STRIP);
    g_assert_nonnull(restored_ws->strip_state);
    g_assert_cmpuint(restored_ws->pane_notebooks->len, ==, 3);

    restored_state = restored_ws->strip_state;
    g_assert_cmpuint(restored_state->columns->len, ==, 3);

    col0 = g_ptr_array_index(restored_state->columns, 0);
    col1 = g_ptr_array_index(restored_state->columns, 1);
    col2 = g_ptr_array_index(restored_state->columns, 2);

    g_assert_cmpint(col0->target_width, ==, 480);
    g_assert_cmpint(col1->target_width, ==, 720);
    g_assert_cmpint(col2->target_width, ==, 960);
    g_assert_true(col0->maximized);
    g_assert_false(col1->maximized);
    g_assert_true(col2->maximized);
    g_assert_cmpint(restored_state->focused_col, ==, 2);

    g_assert_cmpint(strip_apply_layout_calls, ==, 1);
    g_assert_cmpint(strip_focus_column_calls, ==, 1);

    test_ui_free(&ui);
}

static void
test_save_and_restore_classic_keeps_schema_safe(void)
{
    SessionTestUi ui;
    JsonObject *saved_root;
    JsonArray *saved_workspaces;
    JsonObject *saved_ws_obj;
    Workspace *restored_ws;

    test_reset_workspaces();
    test_clear_session_file_for_instance(NULL);
    ui = test_ui_new();

    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Classic WS"));

    session_save(ui.window, ui.browser_notebook, ui.terminal_stack, ui.workspace_list);

    saved_root = test_load_saved_session_for_instance(NULL);
    saved_workspaces = json_object_get_array_member(saved_root, "workspaces");
    saved_ws_obj = json_array_get_object_element(saved_workspaces, 0);

    g_assert_cmpstr(json_object_get_string_member(saved_ws_obj, "layoutMode"), ==, "classic");
    g_assert_false(json_object_has_member(saved_ws_obj, "stripState"));

    json_object_unref(saved_root);

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_STRIP, 2, "Bootstrap"));

    session_restore(ui.window,
                    ui.browser_notebook,
                    ui.terminal_stack,
                    ui.workspace_list,
                    NULL,
                    NULL);

    g_assert_cmpuint(workspaces->len, ==, 1);
    restored_ws = g_ptr_array_index(workspaces, 0);
    g_assert_cmpint(restored_ws->layout_mode, ==, WORKSPACE_LAYOUT_CLASSIC);
    g_assert_null(restored_ws->strip_state);
    g_assert_cmpint(strip_apply_layout_calls, ==, 0);
    g_assert_cmpint(strip_focus_column_calls, ==, 0);

    test_ui_free(&ui);
}

static void
test_save_and_restore_strip_with_stacked_column_round_trip(void)
{
    SessionTestUi ui;
    Workspace *saved_ws;
    WorkspaceStripState *saved_state;
    WorkspaceColumn *col0;
    WorkspaceColumn *col1;
    WorkspaceColumn *col2;
    JsonObject *saved_root;
    JsonArray *saved_workspaces;
    JsonObject *saved_ws_obj;
    JsonObject *saved_strip_obj;
    JsonArray *saved_columns;
    JsonArray *saved_col0_pane_ids;
    Workspace *restored_ws;
    WorkspaceStripState *restored_state;

    test_reset_workspaces();
    test_clear_session_file_for_instance(NULL);
    ui = test_ui_new();

    saved_ws = test_workspace_new(WORKSPACE_LAYOUT_STRIP, 4, "Stacked Strip WS");
    saved_state = saved_ws->strip_state;
    g_ptr_array_add(workspaces, saved_ws);

    g_ptr_array_set_size(saved_state->columns, 0);

    col0 = g_new0(WorkspaceColumn, 1);
    col0->panes = g_ptr_array_new();
    g_ptr_array_add(col0->panes, g_ptr_array_index(saved_ws->pane_notebooks, 0));
    g_ptr_array_add(col0->panes, g_ptr_array_index(saved_ws->pane_notebooks, 1));
    col0->focused_pane = 1;
    col0->target_width = 640;
    col0->current_width = 640.0;
    col0->maximized = FALSE;
    col0->notebook = g_ptr_array_index(col0->panes, 0);
    g_ptr_array_add(saved_state->columns, col0);

    col1 = g_new0(WorkspaceColumn, 1);
    col1->panes = g_ptr_array_new();
    g_ptr_array_add(col1->panes, g_ptr_array_index(saved_ws->pane_notebooks, 2));
    col1->focused_pane = 0;
    col1->target_width = 700;
    col1->current_width = 700.0;
    col1->maximized = TRUE;
    col1->notebook = g_ptr_array_index(col1->panes, 0);
    g_ptr_array_add(saved_state->columns, col1);

    col2 = g_new0(WorkspaceColumn, 1);
    col2->panes = g_ptr_array_new();
    g_ptr_array_add(col2->panes, g_ptr_array_index(saved_ws->pane_notebooks, 3));
    col2->focused_pane = 0;
    col2->target_width = 820;
    col2->current_width = 820.0;
    col2->maximized = FALSE;
    col2->notebook = g_ptr_array_index(col2->panes, 0);
    g_ptr_array_add(saved_state->columns, col2);

    saved_state->focused_col = 1;

    session_save(ui.window, ui.browser_notebook, ui.terminal_stack, ui.workspace_list);

    saved_root = test_load_saved_session_for_instance(NULL);
    saved_workspaces = json_object_get_array_member(saved_root, "workspaces");
    saved_ws_obj = json_array_get_object_element(saved_workspaces, 0);
    saved_strip_obj = json_object_get_object_member(saved_ws_obj, "stripState");
    saved_columns = json_object_get_array_member(saved_strip_obj, "columns");
    saved_col0_pane_ids = json_object_get_array_member(
        json_array_get_object_element(saved_columns, 0), "paneIds");

    g_assert_cmpuint(json_array_get_length(saved_columns), ==, 3);
    g_assert_cmpuint(json_array_get_length(saved_col0_pane_ids), ==, 2);
    g_assert_cmpint((int)json_object_get_int_member(
                        json_array_get_object_element(saved_columns, 0), "focusedPane"),
                    ==, 1);
    g_assert_cmpint((int)json_object_get_int_member(saved_strip_obj, "focusedColumn"), ==, 1);

    json_object_unref(saved_root);

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Bootstrap"));

    session_restore(ui.window,
                    ui.browser_notebook,
                    ui.terminal_stack,
                    ui.workspace_list,
                    NULL,
                    NULL);

    restored_ws = g_ptr_array_index(workspaces, 0);
    restored_state = restored_ws->strip_state;

    g_assert_nonnull(restored_state);
    g_assert_cmpint(restored_ws->layout_mode, ==, WORKSPACE_LAYOUT_STRIP);
    g_assert_cmpuint(restored_ws->pane_notebooks->len, ==, 4);
    g_assert_cmpuint(restored_state->columns->len, ==, 3);
    g_assert_cmpuint(((WorkspaceColumn *)g_ptr_array_index(restored_state->columns, 0))->panes->len, ==, 2);
    g_assert_cmpuint(((WorkspaceColumn *)g_ptr_array_index(restored_state->columns, 1))->panes->len, ==, 1);
    g_assert_cmpuint(((WorkspaceColumn *)g_ptr_array_index(restored_state->columns, 2))->panes->len, ==, 1);

    g_assert_cmpint(((WorkspaceColumn *)g_ptr_array_index(restored_state->columns, 0))->focused_pane, ==, 1);
    g_assert_cmpint(((WorkspaceColumn *)g_ptr_array_index(restored_state->columns, 0))->target_width, ==, 640);
    g_assert_cmpint(((WorkspaceColumn *)g_ptr_array_index(restored_state->columns, 1))->target_width, ==, 700);
    g_assert_true(((WorkspaceColumn *)g_ptr_array_index(restored_state->columns, 1))->maximized);
    g_assert_cmpint(restored_state->focused_col, ==, 1);

    test_ui_free(&ui);
}

static void
test_instance_session_path_is_sanitized_and_distinct(void)
{
    char *default_path = session_get_instance_session_path(NULL);
    char *alpha_path = session_get_instance_session_path("alpha");
    char *alpha_sanitized = session_get_instance_session_path("al!ph@a");

    g_assert_nonnull(default_path);
    g_assert_nonnull(alpha_path);
    g_assert_nonnull(alpha_sanitized);

    g_assert_true(g_str_has_suffix(default_path,
                                   "/.prettymux/sessions/last-default.json"));
    g_assert_true(g_str_has_suffix(alpha_path, "/.prettymux/sessions/last-alpha.json"));
    g_assert_true(g_str_has_suffix(alpha_sanitized, "/.prettymux/sessions/last-alpha.json"));
    g_assert_cmpstr(default_path, !=, alpha_path);

    g_free(default_path);
    g_free(alpha_path);
    g_free(alpha_sanitized);
}

static void
test_session_save_restore_isolated_per_instance(void)
{
    const char *instance_alpha = "phase6-alpha";
    const char *instance_beta = "phase6-beta";
    SessionTestUi ui;
    JsonObject *alpha_json;
    JsonObject *beta_json;
    JsonArray *workspaces_json;
    JsonObject *ws_json;
    Workspace *restored_ws;

    test_clear_session_file_for_instance(instance_alpha);
    test_clear_session_file_for_instance(instance_beta);
    ui = test_ui_new();

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Alpha Workspace"));
    session_save_for_instance(instance_alpha, ui.window, ui.browser_notebook,
                              ui.terminal_stack, ui.workspace_list);

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Beta Workspace"));
    session_save_for_instance(instance_beta, ui.window, ui.browser_notebook,
                              ui.terminal_stack, ui.workspace_list);

    g_assert_true(session_exists_for_instance(instance_alpha));
    g_assert_true(session_exists_for_instance(instance_beta));

    alpha_json = test_load_saved_session_for_instance(instance_alpha);
    workspaces_json = json_object_get_array_member(alpha_json, "workspaces");
    ws_json = json_array_get_object_element(workspaces_json, 0);
    g_assert_cmpstr(json_object_get_string_member(ws_json, "name"), ==,
                    "Alpha Workspace");
    json_object_unref(alpha_json);

    beta_json = test_load_saved_session_for_instance(instance_beta);
    workspaces_json = json_object_get_array_member(beta_json, "workspaces");
    ws_json = json_array_get_object_element(workspaces_json, 0);
    g_assert_cmpstr(json_object_get_string_member(ws_json, "name"), ==,
                    "Beta Workspace");
    json_object_unref(beta_json);

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Bootstrap"));
    session_restore_for_instance(instance_alpha, ui.window, ui.browser_notebook,
                                 ui.terminal_stack, ui.workspace_list, NULL, NULL);
    g_assert_cmpuint(workspaces->len, ==, 1);
    restored_ws = test_first_workspace();
    g_assert_nonnull(restored_ws);
    g_assert_cmpstr(restored_ws->name, ==, "Alpha Workspace");

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Bootstrap"));
    session_restore_for_instance(instance_beta, ui.window, ui.browser_notebook,
                                 ui.terminal_stack, ui.workspace_list, NULL, NULL);
    g_assert_cmpuint(workspaces->len, ==, 1);
    restored_ws = test_first_workspace();
    g_assert_nonnull(restored_ws);
    g_assert_cmpstr(restored_ws->name, ==, "Beta Workspace");

    test_ui_free(&ui);
}

static void
test_session_same_instance_path_survives_restart(void)
{
    const char *instance_id = "phase6-restartable";
    SessionTestUi ui;
    char *first_path;
    char *second_path;
    Workspace *restored_ws;

    test_clear_session_file_for_instance(instance_id);
    ui = test_ui_new();

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Before Restart"));
    session_save_for_instance(instance_id, ui.window, ui.browser_notebook,
                              ui.terminal_stack, ui.workspace_list);

    first_path = session_get_instance_session_path(instance_id);
    g_assert_true(g_file_test(first_path, G_FILE_TEST_EXISTS));

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Bootstrap"));
    session_restore_for_instance(instance_id, ui.window, ui.browser_notebook,
                                 ui.terminal_stack, ui.workspace_list, NULL, NULL);
    restored_ws = test_first_workspace();
    g_assert_nonnull(restored_ws);
    g_assert_cmpstr(restored_ws->name, ==, "Before Restart");

    snprintf(restored_ws->name, sizeof(restored_ws->name), "%s",
             "After Restart");
    session_save_for_instance(instance_id, ui.window, ui.browser_notebook,
                              ui.terminal_stack, ui.workspace_list);

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Bootstrap"));
    session_restore_for_instance(instance_id, ui.window, ui.browser_notebook,
                                 ui.terminal_stack, ui.workspace_list, NULL, NULL);
    restored_ws = test_first_workspace();
    g_assert_nonnull(restored_ws);
    g_assert_cmpstr(restored_ws->name, ==, "After Restart");

    second_path = session_get_instance_session_path(instance_id);
    g_assert_cmpstr(first_path, ==, second_path);

    g_free(first_path);
    g_free(second_path);
    test_ui_free(&ui);
}

static void
test_non_default_restore_does_not_fallback_to_default_session(void)
{
    const char *instance_id = "phase6-nondefault";
    SessionTestUi ui;
    Workspace *restored_ws;
    char *default_path;
    char *legacy_path;
    char *saved_json = NULL;
    gsize saved_len = 0;

    test_clear_session_file_for_instance(NULL);
    test_clear_session_file_for_instance(instance_id);
    test_clear_legacy_session_file();
    ui = test_ui_new();

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Default Session"));
    session_save_for_instance(NULL, ui.window, ui.browser_notebook,
                              ui.terminal_stack, ui.workspace_list);

    default_path = session_get_instance_session_path(NULL);
    legacy_path = test_legacy_session_path();
    g_assert_true(g_file_get_contents(default_path, &saved_json, &saved_len, NULL));
    g_assert_true(g_file_set_contents(legacy_path, saved_json, saved_len, NULL));
    g_assert_cmpint(g_remove(default_path), ==, 0);
    g_free(saved_json);
    saved_json = NULL;

    g_assert_true(session_exists_for_instance(NULL));
    g_assert_false(session_exists_for_instance(instance_id));

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Bootstrap"));
    session_restore_for_instance(instance_id, ui.window, ui.browser_notebook,
                                 ui.terminal_stack, ui.workspace_list, NULL, NULL);
    restored_ws = test_first_workspace();
    g_assert_nonnull(restored_ws);
    g_assert_cmpstr(restored_ws->name, ==, "Bootstrap");

    g_free(default_path);
    g_free(legacy_path);
    test_ui_free(&ui);
}

static void
test_default_restore_falls_back_to_legacy_session(void)
{
    SessionTestUi ui;
    Workspace *restored_ws;
    char *default_path;
    char *legacy_path;
    char *saved_json = NULL;
    gsize saved_len = 0;

    test_clear_session_file_for_instance(NULL);
    test_clear_legacy_session_file();
    ui = test_ui_new();

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1,
                                       "Legacy Default Session"));
    session_save_for_instance(NULL, ui.window, ui.browser_notebook,
                              ui.terminal_stack, ui.workspace_list);

    default_path = session_get_instance_session_path(NULL);
    legacy_path = test_legacy_session_path();
    g_assert_true(g_file_get_contents(default_path, &saved_json, &saved_len, NULL));
    g_assert_true(g_file_set_contents(legacy_path, saved_json, saved_len, NULL));
    g_assert_cmpint(g_remove(default_path), ==, 0);
    g_free(saved_json);

    test_reset_workspaces();
    g_ptr_array_add(workspaces,
                    test_workspace_new(WORKSPACE_LAYOUT_CLASSIC, 1, "Bootstrap"));
    session_restore_for_instance(NULL, ui.window, ui.browser_notebook,
                                 ui.terminal_stack, ui.workspace_list, NULL, NULL);
    restored_ws = test_first_workspace();
    g_assert_nonnull(restored_ws);
    g_assert_cmpstr(restored_ws->name, ==, "Legacy Default Session");

    g_free(default_path);
    g_free(legacy_path);
    test_ui_free(&ui);
}

int
main(int argc, char **argv)
{
    char *tmp_home;

    gtk_init();
    g_test_init(&argc, &argv, NULL);

    tmp_home = g_dir_make_tmp("prettymux-session-strip-XXXXXX", NULL);
    g_assert_nonnull(tmp_home);
    g_setenv("HOME", tmp_home, TRUE);
    g_unsetenv("PRETTYMUX_INSTANCE_ID");
    g_free(tmp_home);

    g_test_add_func("/session-strip/integration/save-restore-strip-round-trip",
                    test_save_and_restore_strip_round_trip);
    g_test_add_func("/session-strip/integration/save-restore-strip-stacked-round-trip",
                    test_save_and_restore_strip_with_stacked_column_round_trip);
    g_test_add_func("/session-strip/integration/save-restore-classic-safe",
                    test_save_and_restore_classic_keeps_schema_safe);
    g_test_add_func("/session-strip/instance/path-sanitized-distinct",
                    test_instance_session_path_is_sanitized_and_distinct);
    g_test_add_func("/session-strip/instance/save-restore-isolated",
                    test_session_save_restore_isolated_per_instance);
    g_test_add_func("/session-strip/instance/restart-stable-path",
                    test_session_same_instance_path_survives_restart);
    g_test_add_func("/session-strip/instance/nondefault-no-default-fallback",
                    test_non_default_restore_does_not_fallback_to_default_session);
    g_test_add_func("/session-strip/instance/default-legacy-fallback",
                    test_default_restore_falls_back_to_legacy_session);

    return g_test_run();
}
