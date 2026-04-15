#include "session.h"
#include "theme.h"
#include "workspace.h"
#include "workspace_strip.h"
#include "browser_tab.h"
#include "ghostty_terminal.h"
#include "project_icon_cache.h"
#include <json-glib/json-glib.h>
#include <string.h>

#define SESSION_STRIP_DEFAULT_COL_WIDTH 600
#define SESSION_DEFAULT_INSTANCE_ID "default"
#define SESSION_DEFAULT_FILE_NAME "last-default.json"
#define SESSION_LEGACY_FILE_NAME "last.json"

const char *app_state_get_instance_id(void);

static GtkWindow *session_window = NULL;
static GtkWidget *session_browser_notebook = NULL;
static GtkWidget *session_terminal_stack = NULL;
static GtkWidget *session_workspace_list = NULL;
static guint session_save_source_id = 0;
static gboolean session_shutting_down = FALSE;
static gboolean session_restoring = FALSE;
static guint64 session_generated_pane_id = 1;

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

static gboolean
session_instance_id_char_allowed(char c)
{
    return g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.';
}

static char *
session_sanitize_instance_id(const char *instance_id)
{
    GString *sanitized = g_string_new(NULL);

    if (!instance_id || !instance_id[0])
        instance_id = SESSION_DEFAULT_INSTANCE_ID;

    for (const char *p = instance_id; *p; p++) {
        if (session_instance_id_char_allowed(*p))
            g_string_append_c(sanitized, *p);
    }

    if (sanitized->len == 0)
        g_string_assign(sanitized, SESSION_DEFAULT_INSTANCE_ID);

    return g_string_free(sanitized, FALSE);
}

char *
session_get_instance_session_path(const char *instance_id)
{
    char *dir = g_build_filename(g_get_home_dir(), ".prettymux", "sessions",
                                 NULL);
    g_autofree char *safe_instance_id =
        session_sanitize_instance_id(instance_id);
    g_autofree char *file_name = NULL;
    char *path;

    if (g_strcmp0(safe_instance_id, SESSION_DEFAULT_INSTANCE_ID) == 0)
        file_name = g_strdup(SESSION_DEFAULT_FILE_NAME);
    else
        file_name = g_strdup_printf("last-%s.json", safe_instance_id);

    g_mkdir_with_parents(dir, 0755);
    path = g_build_filename(dir, file_name, NULL);
    g_free(dir);
    return path;
}

static char *
session_get_legacy_session_path(void)
{
    char *dir = g_build_filename(g_get_home_dir(), ".prettymux", "sessions",
                                 NULL);
    char *path;

    g_mkdir_with_parents(dir, 0755);
    path = g_build_filename(dir, SESSION_LEGACY_FILE_NAME, NULL);
    g_free(dir);
    return path;
}

static char *
session_get_restore_path_for_instance(const char *instance_id)
{
    char *instance_path = session_get_instance_session_path(instance_id);
    g_autofree char *safe_instance_id =
        session_sanitize_instance_id(instance_id);

    if (g_file_test(instance_path, G_FILE_TEST_EXISTS))
        return instance_path;

    if (g_strcmp0(safe_instance_id, SESSION_DEFAULT_INSTANCE_ID) != 0)
        return instance_path;

    g_autofree char *legacy_path = session_get_legacy_session_path();
    if (g_file_test(legacy_path, G_FILE_TEST_EXISTS)) {
        g_free(instance_path);
        return g_strdup(legacy_path);
    }

    return instance_path;
}

static const char *
session_current_instance_id(void)
{
    return app_state_get_instance_id();
}

gboolean
session_exists_for_instance(const char *instance_id)
{
    g_autofree char *path =
        session_get_restore_path_for_instance(instance_id);
    return g_file_test(path, G_FILE_TEST_EXISTS);
}

gboolean
session_exists(void)
{
    return session_exists_for_instance(session_current_instance_id());
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

static void
session_assign_pane_id(GtkNotebook *pane, const char *pane_id);

static char *
session_next_generated_pane_id(void)
{
    return g_strdup_printf("pane-restored-%" G_GUINT64_FORMAT,
                           session_generated_pane_id++);
}

static void
session_normalize_workspace_pane_ids(Workspace *ws)
{
    GHashTable *seen;

    if (!ws || !ws->pane_notebooks)
        return;

    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < ws->pane_notebooks->len; i++) {
        GtkNotebook *pane = g_ptr_array_index(ws->pane_notebooks, i);
        const char *pane_id = workspace_get_pane_id(pane);
        gboolean needs_new_id = FALSE;
        char *owned_id;

        if (!pane_id || !pane_id[0]) {
            needs_new_id = TRUE;
        } else if (g_hash_table_contains(seen, pane_id)) {
            needs_new_id = TRUE;
        }

        if (needs_new_id) {
            char *fresh_id = session_next_generated_pane_id();
            session_assign_pane_id(pane, fresh_id);
            g_hash_table_add(seen, fresh_id);
        } else {
            owned_id = g_strdup(pane_id);
            g_hash_table_add(seen, owned_id);
        }
    }

    g_hash_table_unref(seen);
}

static void
session_assign_workspace_pane_ids_from_saved_order(Workspace *ws,
                                                   JsonArray *panes_arr)
{
    GHashTable *seen;
    guint n_panes;

    if (!ws || !ws->pane_notebooks || !panes_arr)
        return;

    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    n_panes = json_array_get_length(panes_arr);

    for (guint i = 0; i < n_panes && i < ws->pane_notebooks->len; i++) {
        JsonNode *pane_node = json_array_get_element(panes_arr, i);
        JsonObject *pane_obj;
        const char *saved_pane_id;
        char *assigned_id = NULL;

        if (!pane_node || !JSON_NODE_HOLDS_OBJECT(pane_node))
            continue;

        pane_obj = json_node_get_object(pane_node);
        saved_pane_id = json_object_get_string_member_with_default(
            pane_obj, "paneId", "");

        if (saved_pane_id[0] && !g_hash_table_contains(seen, saved_pane_id))
            assigned_id = g_strdup(saved_pane_id);
        else
            assigned_id = session_next_generated_pane_id();

        session_assign_pane_id(g_ptr_array_index(ws->pane_notebooks, i),
                               assigned_id);
        g_hash_table_add(seen, assigned_id);
    }

    for (guint i = n_panes; i < ws->pane_notebooks->len; i++) {
        char *fresh_id = session_next_generated_pane_id();
        session_assign_pane_id(g_ptr_array_index(ws->pane_notebooks, i),
                               fresh_id);
        g_hash_table_add(seen, fresh_id);
    }

    g_hash_table_unref(seen);
}

static WorkspaceLayoutMode
session_parse_workspace_layout_mode(JsonObject *ws_obj)
{
    const char *layout_mode_name;

    if (!ws_obj)
        return WORKSPACE_LAYOUT_CLASSIC;

    layout_mode_name = json_object_get_string_member_with_default(
        ws_obj, "layoutMode", "classic");
    if (g_strcmp0(layout_mode_name, "strip") == 0)
        return WORKSPACE_LAYOUT_STRIP;

    return WORKSPACE_LAYOUT_CLASSIC;
}

static int
session_strip_column_pane_index_by_pane_id(WorkspaceColumn *col,
                                           const char *pane_id)
{
    if (!col || !pane_id || !pane_id[0] || !col->panes)
        return -1;

    for (guint i = 0; i < col->panes->len; i++) {
        GtkWidget *pane_widget = g_ptr_array_index(col->panes, i);
        const char *col_pane_id;

        if (!GTK_IS_NOTEBOOK(pane_widget))
            continue;
        col_pane_id = workspace_get_pane_id(GTK_NOTEBOOK(pane_widget));
        if (g_strcmp0(col_pane_id, pane_id) == 0)
            return (int)i;
    }

    return -1;
}

static int
session_strip_column_index_by_pane_id(Workspace *ws, const char *pane_id)
{
    WorkspaceStripState *state;

    if (!ws || !ws->strip_state || !pane_id || !pane_id[0])
        return -1;

    state = ws->strip_state;
    for (guint i = 0; i < state->columns->len; i++) {
        WorkspaceColumn *col = g_ptr_array_index(state->columns, i);
        int pane_idx;

        if (!col)
            continue;

        pane_idx = session_strip_column_pane_index_by_pane_id(col, pane_id);
        if (pane_idx >= 0)
            return (int)i;

        if (GTK_IS_NOTEBOOK(col->notebook)) {
            const char *col_pane_id =
                workspace_get_pane_id(GTK_NOTEBOOK(col->notebook));
            if (g_strcmp0(col_pane_id, pane_id) == 0)
                return (int)i;
        }
    }

    return -1;
}

static GtkWidget *
session_strip_build_column_root(GPtrArray *panes)
{
    GtkWidget *root;

    if (!panes || panes->len == 0)
        return NULL;

    root = g_ptr_array_index(panes, 0);
    for (guint i = 1; i < panes->len; i++) {
        GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

        gtk_widget_set_hexpand(paned, TRUE);
        gtk_widget_set_vexpand(paned, TRUE);
        gtk_paned_set_start_child(GTK_PANED(paned), root);
        gtk_paned_set_end_child(GTK_PANED(paned), g_ptr_array_index(panes, i));
        gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
        gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
        gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
        gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
        root = paned;
    }

    return root;
}

static void
session_strip_detach_widget_from_parent(GtkWidget *widget)
{
    GtkWidget *parent;

    if (!widget)
        return;

    parent = gtk_widget_get_parent(widget);
    if (!parent)
        return;

    if (GTK_IS_PANED(parent)) {
        if (gtk_paned_get_start_child(GTK_PANED(parent)) == widget)
            gtk_paned_set_start_child(GTK_PANED(parent), NULL);
        else if (gtk_paned_get_end_child(GTK_PANED(parent)) == widget)
            gtk_paned_set_end_child(GTK_PANED(parent), NULL);
        return;
    }

    if (GTK_IS_BOX(parent)) {
        gtk_box_remove(GTK_BOX(parent), widget);
        return;
    }

    if (GTK_IS_OVERLAY(parent)) {
        if (gtk_overlay_get_child(GTK_OVERLAY(parent)) == widget)
            gtk_overlay_set_child(GTK_OVERLAY(parent), NULL);
        else
            gtk_overlay_remove_overlay(GTK_OVERLAY(parent), widget);
        return;
    }

    if (GTK_IS_STACK(parent)) {
        gtk_stack_remove(GTK_STACK(parent), widget);
    }
}

static void
session_strip_rebuild_columns_from_saved_state(Workspace *ws,
                                               JsonObject *strip_obj)
{
    WorkspaceStripState *state;
    JsonArray *columns_arr;
    guint columns_len;
    g_autoptr(GHashTable) seen_panes = NULL;
    g_autoptr(GPtrArray) rebuilt_columns = NULL;

    if (!ws || !strip_obj || !json_object_has_member(strip_obj, "columns"))
        return;

    state = ws->strip_state;
    if (!state || !state->columns)
        return;

    columns_arr = json_object_get_array_member(strip_obj, "columns");
    if (!columns_arr)
        return;
    columns_len = json_array_get_length(columns_arr);
    if (columns_len == 0)
        return;

    seen_panes = g_hash_table_new(g_direct_hash, g_direct_equal);
    rebuilt_columns = g_ptr_array_new();

    for (guint i = 0; i < columns_len; i++) {
        JsonNode *column_node = json_array_get_element(columns_arr, i);
        JsonObject *column_obj;
        g_autoptr(GPtrArray) col_panes = NULL;
        WorkspaceColumn *col;
        int focused_pane = 0;
        int width = SESSION_STRIP_DEFAULT_COL_WIDTH;
        gboolean maximized = FALSE;

        if (!column_node || !JSON_NODE_HOLDS_OBJECT(column_node))
            continue;
        column_obj = json_node_get_object(column_node);
        col_panes = g_ptr_array_new();

        if (json_object_has_member(column_obj, "paneIds")) {
            JsonArray *pane_ids_arr =
                json_object_get_array_member(column_obj, "paneIds");
            guint pane_ids_len = pane_ids_arr ? json_array_get_length(pane_ids_arr)
                                              : 0;

            for (guint pi = 0; pi < pane_ids_len; pi++) {
                const char *pane_id =
                    json_array_get_string_element(pane_ids_arr, pi);
                GtkNotebook *pane;

                if (!pane_id || !pane_id[0])
                    continue;
                pane = workspace_get_pane_by_id(ws, pane_id);
                if (!pane || g_hash_table_contains(seen_panes, pane))
                    continue;

                g_ptr_array_add(col_panes, pane);
                g_hash_table_add(seen_panes, pane);
            }
        } else {
            const char *pane_id =
                json_object_get_string_member_with_default(column_obj,
                                                           "paneId", "");
            GtkNotebook *pane = workspace_get_pane_by_id(ws, pane_id);

            if (pane && !g_hash_table_contains(seen_panes, pane)) {
                g_ptr_array_add(col_panes, pane);
                g_hash_table_add(seen_panes, pane);
            }
        }

        if (col_panes->len == 0)
            continue;

        if (state->column_box) {
            for (guint pi = 0; pi < col_panes->len; pi++) {
                GtkWidget *pane_widget = g_ptr_array_index(col_panes, pi);
                session_strip_detach_widget_from_parent(pane_widget);
            }
        }

        if (json_object_has_member(column_obj, "focusedPaneId")) {
            const char *focused_pane_id =
                json_object_get_string_member_with_default(column_obj,
                                                           "focusedPaneId", "");
            for (guint pi = 0; pi < col_panes->len; pi++) {
                GtkNotebook *pane = g_ptr_array_index(col_panes, pi);
                const char *pane_id = workspace_get_pane_id(pane);

                if (g_strcmp0(pane_id, focused_pane_id) == 0) {
                    focused_pane = (int)pi;
                    break;
                }
            }
        } else if (json_object_has_member(column_obj, "focusedPane")) {
            focused_pane = (int)json_object_get_int_member_with_default(
                column_obj, "focusedPane", 0);
        }
        if (focused_pane < 0 || focused_pane >= (int)col_panes->len)
            focused_pane = 0;

        width = (int)json_object_get_int_member_with_default(column_obj,
                                                             "width", width);
        if (width <= 0)
            width = SESSION_STRIP_DEFAULT_COL_WIDTH;
        maximized = json_object_get_boolean_member_with_default(column_obj,
                                                                "maximized",
                                                                FALSE);

        col = g_new0(WorkspaceColumn, 1);
        col->panes = g_ptr_array_ref(col_panes);
        col->focused_pane = focused_pane;
        col->target_width = width;
        col->current_width = (double)width;
        col->maximized = maximized;
        if (state->column_box)
            col->notebook = session_strip_build_column_root(col->panes);
        else
            col->notebook = g_ptr_array_index(col->panes, 0);
        if (!col->notebook) {
            g_ptr_array_unref(col->panes);
            g_free(col);
            continue;
        }
        g_ptr_array_add(rebuilt_columns, col);
    }

    if (ws->pane_notebooks) {
        for (guint i = 0; i < ws->pane_notebooks->len; i++) {
            GtkNotebook *pane = g_ptr_array_index(ws->pane_notebooks, i);
            WorkspaceColumn *col;

            if (!pane || g_hash_table_contains(seen_panes, pane))
                continue;

            col = g_new0(WorkspaceColumn, 1);
            col->panes = g_ptr_array_new();
            g_ptr_array_add(col->panes, pane);
            col->focused_pane = 0;
            col->target_width = SESSION_STRIP_DEFAULT_COL_WIDTH;
            col->current_width = (double)SESSION_STRIP_DEFAULT_COL_WIDTH;
            col->maximized = FALSE;
            col->notebook = GTK_WIDGET(pane);
            g_ptr_array_add(rebuilt_columns, col);
            g_hash_table_add(seen_panes, pane);
        }
    }

    if (rebuilt_columns->len == 0)
        return;

    if (state->column_box) {
        for (guint i = 0; i < state->columns->len; i++) {
            WorkspaceColumn *old_col = g_ptr_array_index(state->columns, i);
            if (!old_col || !old_col->notebook)
                continue;
            if (gtk_widget_get_parent(old_col->notebook) == state->column_box) {
                gtk_box_remove(GTK_BOX(state->column_box), old_col->notebook);
            }
        }
    }

    g_ptr_array_set_size(state->columns, 0);
    for (guint i = 0; i < rebuilt_columns->len; i++) {
        WorkspaceColumn *col = g_ptr_array_index(rebuilt_columns, i);
        g_ptr_array_add(state->columns, col);
        if (state->column_box && col->notebook &&
            gtk_widget_get_parent(col->notebook) != state->column_box) {
            gtk_box_append(GTK_BOX(state->column_box), col->notebook);
        }
    }

    if (ws->pane_notebooks) {
        g_ptr_array_set_size(ws->pane_notebooks, 0);
        for (guint i = 0; i < state->columns->len; i++) {
            WorkspaceColumn *col = g_ptr_array_index(state->columns, i);
            if (!col || !col->panes)
                continue;
            for (guint pi = 0; pi < col->panes->len; pi++) {
                g_ptr_array_add(ws->pane_notebooks,
                                g_ptr_array_index(col->panes, pi));
            }
        }
        if (ws->pane_notebooks->len > 0)
            ws->notebook = g_ptr_array_index(ws->pane_notebooks, 0);
    }

    g_ptr_array_set_size(rebuilt_columns, 0);
}

static void
session_save_workspace_layout_mode(JsonBuilder *builder, Workspace *ws)
{
    const char *layout_mode_name = "classic";

    if (ws && workspace_get_layout_mode(ws) == WORKSPACE_LAYOUT_STRIP)
        layout_mode_name = "strip";

    json_builder_set_member_name(builder, "layoutMode");
    json_builder_add_string_value(builder, layout_mode_name);
}

static void
session_save_workspace_strip_state(JsonBuilder *builder, Workspace *ws)
{
    WorkspaceStripState *state;
    int focused_col = 0;
    int maximized_col = -1;
    const char *focused_pane_id = "";
    const char *maximized_pane_id = "";

    if (!builder || !ws || workspace_get_layout_mode(ws) != WORKSPACE_LAYOUT_STRIP)
        return;

    state = ws->strip_state;
    if (state && state->focused_col >= 0)
        focused_col = state->focused_col;

    json_builder_set_member_name(builder, "stripState");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "columns");
    json_builder_begin_array(builder);
    if (state && state->columns && state->columns->len > 0) {
        for (guint i = 0; i < state->columns->len; i++) {
            WorkspaceColumn *col = g_ptr_array_index(state->columns, i);
            GtkNotebook *focused_nb;
            const char *column_pane_id = "";
            const char *column_focused_pane_id = "";
            int width = SESSION_STRIP_DEFAULT_COL_WIDTH;
            gboolean maximized = FALSE;
            int focused_pane = 0;

            if (!col)
                continue;

            focused_nb = workspace_strip_column_focused_notebook(col);
            if (focused_nb)
                column_focused_pane_id = workspace_get_pane_id(focused_nb);
            if (!column_focused_pane_id)
                column_focused_pane_id = "";
            column_pane_id = column_focused_pane_id;

            if (col->target_width > 0)
                width = col->target_width;
            maximized = col->maximized;

            if (maximized && maximized_col < 0) {
                maximized_col = (int)i;
                maximized_pane_id = column_pane_id;
            }
            if ((int)i == focused_col)
                focused_pane_id = column_pane_id;

            if (col->panes && col->panes->len > 0) {
                focused_pane = CLAMP(col->focused_pane, 0,
                                     (int)col->panes->len - 1);
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "paneId");
            json_builder_add_string_value(builder, column_pane_id);
            json_builder_set_member_name(builder, "paneIds");
            json_builder_begin_array(builder);
            if (col->panes && col->panes->len > 0) {
                for (guint pi = 0; pi < col->panes->len; pi++) {
                    GtkWidget *pane_widget = g_ptr_array_index(col->panes, pi);
                    const char *pane_id = "";

                    if (GTK_IS_NOTEBOOK(pane_widget))
                        pane_id = workspace_get_pane_id(
                            GTK_NOTEBOOK(pane_widget));
                    json_builder_add_string_value(builder,
                                                  pane_id ? pane_id : "");
                }
            } else {
                json_builder_add_string_value(builder, column_pane_id);
            }
            json_builder_end_array(builder);
            json_builder_set_member_name(builder, "focusedPane");
            json_builder_add_int_value(builder, focused_pane);
            json_builder_set_member_name(builder, "focusedPaneId");
            if (col->panes && focused_pane >= 0 &&
                focused_pane < (int)col->panes->len &&
                GTK_IS_NOTEBOOK(g_ptr_array_index(col->panes, focused_pane))) {
                const char *pane_id = workspace_get_pane_id(
                    GTK_NOTEBOOK(g_ptr_array_index(col->panes, focused_pane)));
                json_builder_add_string_value(builder, pane_id ? pane_id : "");
            } else {
                json_builder_add_string_value(builder, column_pane_id);
            }
            json_builder_set_member_name(builder, "width");
            json_builder_add_int_value(builder, width);
            json_builder_set_member_name(builder, "maximized");
            json_builder_add_boolean_value(builder, maximized);
            json_builder_end_object(builder);
        }
    } else if (ws->pane_notebooks) {
        for (guint i = 0; i < ws->pane_notebooks->len; i++) {
            GtkNotebook *pane = g_ptr_array_index(ws->pane_notebooks, i);
            const char *pane_id = workspace_get_pane_id(pane);
            int width = SESSION_STRIP_DEFAULT_COL_WIDTH;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "paneId");
            json_builder_add_string_value(builder, pane_id ? pane_id : "");
            json_builder_set_member_name(builder, "paneIds");
            json_builder_begin_array(builder);
            json_builder_add_string_value(builder, pane_id ? pane_id : "");
            json_builder_end_array(builder);
            json_builder_set_member_name(builder, "focusedPane");
            json_builder_add_int_value(builder, 0);
            json_builder_set_member_name(builder, "focusedPaneId");
            json_builder_add_string_value(builder, pane_id ? pane_id : "");
            json_builder_set_member_name(builder, "width");
            json_builder_add_int_value(builder, width);
            json_builder_set_member_name(builder, "maximized");
            json_builder_add_boolean_value(builder, FALSE);
            json_builder_end_object(builder);
        }
    }
    json_builder_end_array(builder);

    if (!state || !state->columns || focused_col < 0 ||
        focused_col >= (int)state->columns->len) {
        focused_col = 0;
        focused_pane_id = "";
    }

    json_builder_set_member_name(builder, "focusedColumn");
    json_builder_add_int_value(builder, focused_col);
    json_builder_set_member_name(builder, "focusedPaneId");
    json_builder_add_string_value(builder, focused_pane_id);

    json_builder_set_member_name(builder, "maximizedColumn");
    json_builder_add_int_value(builder, maximized_col);
    json_builder_set_member_name(builder, "maximizedPaneId");
    json_builder_add_string_value(builder, maximized_pane_id);
    json_builder_end_object(builder);
}

static void
session_restore_workspace_strip_state(Workspace *ws, JsonObject *ws_obj)
{
    WorkspaceStripState *state;
    JsonObject *strip_obj;
    int focused_col = 0;
    int legacy_maximized_col = -1;
    gboolean focused_from_pane = FALSE;
    gboolean any_column_maximized = FALSE;

    if (!ws || !ws_obj || workspace_get_layout_mode(ws) != WORKSPACE_LAYOUT_STRIP)
        return;

    state = ws->strip_state;
    if (!state || !state->columns || state->columns->len == 0)
        return;
    if (!json_object_has_member(ws_obj, "stripState"))
        return;

    strip_obj = json_object_get_object_member(ws_obj, "stripState");
    if (!strip_obj)
        return;

    session_strip_rebuild_columns_from_saved_state(ws, strip_obj);
    state = ws->strip_state;
    if (!state || !state->columns || state->columns->len == 0)
        return;

    for (guint i = 0; i < state->columns->len; i++) {
        WorkspaceColumn *col = g_ptr_array_index(state->columns, i);
        if (!col)
            continue;
        if (col->target_width <= 0)
            col->target_width = SESSION_STRIP_DEFAULT_COL_WIDTH;
        col->current_width = (double)col->target_width;
        col->maximized = FALSE;
    }

    if (json_object_has_member(strip_obj, "columns")) {
        JsonArray *columns_arr = json_object_get_array_member(strip_obj, "columns");
        guint columns_len = json_array_get_length(columns_arr);

        for (guint i = 0; i < columns_len; i++) {
            JsonNode *column_node = json_array_get_element(columns_arr, i);
            JsonObject *column_obj;
            const char *pane_id;
            int col_idx = -1;
            int width;
            gboolean maximized;
            gboolean has_maximized_member;
            WorkspaceColumn *col;
            int focused_pane = 0;

            if (!column_node || !JSON_NODE_HOLDS_OBJECT(column_node))
                continue;
            column_obj = json_node_get_object(column_node);
            pane_id = json_object_get_string_member_with_default(
                column_obj, "paneId", "");
            if (pane_id[0])
                col_idx = session_strip_column_index_by_pane_id(ws, pane_id);
            if (col_idx < 0 && i < state->columns->len)
                col_idx = (int)i;
            if (col_idx < 0 || col_idx >= (int)state->columns->len)
                continue;

            col = g_ptr_array_index(state->columns, col_idx);
            if (!col)
                continue;

            width = (int)json_object_get_int_member_with_default(
                column_obj, "width", col->target_width);
            if (width <= 0)
                width = SESSION_STRIP_DEFAULT_COL_WIDTH;
            col->target_width = width;
            col->current_width = (double)width;

            has_maximized_member = json_object_has_member(column_obj,
                                                          "maximized");
            maximized = json_object_get_boolean_member_with_default(
                column_obj, "maximized", FALSE);
            if (has_maximized_member) {
                col->maximized = maximized;
                if (maximized)
                    any_column_maximized = TRUE;
            }

            if (json_object_has_member(column_obj, "focusedPaneId")) {
                const char *focused_pane_id =
                    json_object_get_string_member_with_default(
                        column_obj, "focusedPaneId", "");
                int pane_idx =
                    session_strip_column_pane_index_by_pane_id(col,
                                                               focused_pane_id);
                if (pane_idx >= 0)
                    focused_pane = pane_idx;
            } else if (json_object_has_member(column_obj, "focusedPane")) {
                focused_pane = (int)json_object_get_int_member_with_default(
                    column_obj, "focusedPane", col->focused_pane);
            } else {
                focused_pane = col->focused_pane;
            }

            if (col->panes && col->panes->len > 0) {
                if (focused_pane < 0 || focused_pane >= (int)col->panes->len)
                    focused_pane = 0;
                col->focused_pane = focused_pane;
            } else {
                col->focused_pane = 0;
            }
        }
    }

    if (json_object_has_member(strip_obj, "focusedPaneId")) {
        const char *focused_pane_id =
            json_object_get_string_member_with_default(strip_obj,
                                                       "focusedPaneId", "");
        if (focused_pane_id[0]) {
            int focused_by_pane =
                session_strip_column_index_by_pane_id(ws, focused_pane_id);
            if (focused_by_pane >= 0) {
                WorkspaceColumn *focused_column =
                    g_ptr_array_index(state->columns, focused_by_pane);
                int pane_idx = session_strip_column_pane_index_by_pane_id(
                    focused_column, focused_pane_id);
                if (pane_idx >= 0)
                    focused_column->focused_pane = pane_idx;
                focused_col = focused_by_pane;
                focused_from_pane = TRUE;
            }
        }
    }
    if (!focused_from_pane) {
        focused_col = (int)json_object_get_int_member_with_default(
            strip_obj, "focusedColumn", state->focused_col);
    }
    if (focused_col < 0 || focused_col >= (int)state->columns->len)
        focused_col = 0;

    if (!any_column_maximized && json_object_has_member(strip_obj, "maximizedPaneId")) {
        const char *maximized_pane_id =
            json_object_get_string_member_with_default(strip_obj,
                                                       "maximizedPaneId", "");
        if (maximized_pane_id[0]) {
            int maximized_by_pane =
                session_strip_column_index_by_pane_id(ws, maximized_pane_id);
            if (maximized_by_pane >= 0)
                legacy_maximized_col = maximized_by_pane;
        }
    }
    if (!any_column_maximized &&
        legacy_maximized_col < 0 &&
        json_object_has_member(strip_obj, "maximizedColumn")) {
        int saved_maximized_col = (int)json_object_get_int_member_with_default(
            strip_obj, "maximizedColumn", legacy_maximized_col);
        if (saved_maximized_col >= 0 &&
            saved_maximized_col < (int)state->columns->len)
            legacy_maximized_col = saved_maximized_col;
    }

    if (!any_column_maximized &&
        legacy_maximized_col >= 0 &&
        legacy_maximized_col < (int)state->columns->len) {
        WorkspaceColumn *maximized = g_ptr_array_index(state->columns,
                                                       legacy_maximized_col);
        if (maximized)
            maximized->maximized = TRUE;
    }

    state->focused_col = focused_col;
    workspace_strip_apply_layout(ws);
    workspace_strip_focus_column(ws, focused_col);
}

#ifdef PRETTYMUX_TEST_HOOKS
void
session_test_save_workspace_layout_mode(JsonBuilder *builder, Workspace *ws)
{
    session_save_workspace_layout_mode(builder, ws);
}

void
session_test_save_workspace_strip_state(JsonBuilder *builder, Workspace *ws)
{
    session_save_workspace_strip_state(builder, ws);
}

void
session_test_restore_workspace_strip_state(Workspace *ws, JsonObject *ws_obj)
{
    session_restore_workspace_strip_state(ws, ws_obj);
}
#endif

static gboolean
session_finish_restore_cb(gpointer data)
{
    (void)data;
    session_restoring = FALSE;
    session_queue_save();
    return G_SOURCE_REMOVE;
}

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
        const char *pane_id;
        pane_id = workspace_get_pane_id(GTK_NOTEBOOK(widget));

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

    if (size > 10) {
        position = (int)(restore->ratio * (double)size);
        if (position < 0)
            position = 0;
        if (position > size)
            position = size;
        gtk_paned_set_position(restore->paned, position);

        g_object_unref(restore->paned);
        g_free(restore);
        return G_SOURCE_REMOVE;
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
        GtkPaned *this_paned = NULL;
        double ratio = json_object_get_double_member_with_default(layout_obj,
                                                                  "ratio",
                                                                  0.5);

        if (!start_obj || !end_obj)
            return FALSE;

        new_pane = workspace_split_pane_target(ws, seed_pane, orientation,
                                               ghostty_app);
        if (!GTK_IS_NOTEBOOK(new_pane))
            return FALSE;

        parent = gtk_widget_get_parent(GTK_WIDGET(seed_pane));
        if (GTK_IS_PANED(parent))
            this_paned = g_object_ref(GTK_PANED(parent));

        if (!session_restore_workspace_layout_node(ws, start_obj, seed_pane,
                                                   ghostty_app)) {
            if (this_paned)
                g_object_unref(this_paned);
            return FALSE;
        }
        if (!session_restore_workspace_layout_node(ws, end_obj, new_pane,
                                                   ghostty_app)) {
            if (this_paned)
                g_object_unref(this_paned);
            return FALSE;
        }

        if (this_paned) {
            session_schedule_split_restore(this_paned, ratio);
            g_object_unref(this_paned);
        }

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
    if (session_shutting_down || session_restoring)
        return;

    if (!session_window || !session_browser_notebook ||
        !session_terminal_stack || !session_workspace_list)
        return;

    if (session_save_source_id != 0)
        return;

    session_save_source_id = g_idle_add(session_save_idle_cb, NULL);
}

void
session_save_for_instance(const char *instance_id,
                          GtkWindow *window,
                          GtkWidget *browser_notebook,
                          GtkWidget *terminal_stack,
                          GtkWidget *workspace_list)
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

            session_normalize_workspace_pane_ids(ws);

            json_builder_set_member_name(b, "name");
            json_builder_add_string_value(b, ws->name);

            json_builder_set_member_name(b, "notes");
            json_builder_add_string_value(b,
                ws->notes_text ? ws->notes_text : "");

            session_save_workspace_layout_mode(b, ws);

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

            session_save_workspace_strip_state(b, ws);

            json_builder_end_object(b);
        }
    }
    json_builder_end_array(b);

    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, root);

    char *path = session_get_instance_session_path(instance_id);
    json_generator_to_file(gen, path, NULL);
    g_free(path);

    json_node_unref(root);
    g_object_unref(gen);
    g_object_unref(b);
}

void
session_save(GtkWindow *window, GtkWidget *browser_notebook,
             GtkWidget *terminal_stack, GtkWidget *workspace_list)
{
    session_save_for_instance(session_current_instance_id(), window,
                              browser_notebook, terminal_stack, workspace_list);
}

void
session_restore_for_instance(const char *instance_id,
                             GtkWindow *window,
                             GtkWidget *browser_notebook,
                             GtkWidget *terminal_stack,
                             GtkWidget *workspace_list,
                             ghostty_app_t ghostty_app,
                             SessionAddBrowserTabFunc add_browser_tab_func)
{
    char *path = session_get_restore_path_for_instance(instance_id);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return;
    }

    session_restoring = TRUE;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, NULL)) {
        session_restoring = FALSE;
        g_object_unref(parser);
        g_free(path);
        return;
    }
    g_free(path);

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        session_restoring = FALSE;
        g_object_unref(parser);
        return;
    }

    JsonObject *obj = json_node_get_object(root);

    if (json_object_get_int_member_with_default(obj, "version", 0) != 1) {
        session_restoring = FALSE;
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
            WorkspaceLayoutMode saved_layout_mode =
                session_parse_workspace_layout_mode(ws_obj);
            gboolean strip_mode_active = FALSE;

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

            if (workspace_get_layout_mode(ws) != saved_layout_mode) {
                if (!workspace_rebuild_for_layout_mode(ws, saved_layout_mode))
                    saved_layout_mode = workspace_get_layout_mode(ws);
            }
            strip_mode_active = (saved_layout_mode == WORKSPACE_LAYOUT_STRIP);

            /* Restore panes.  The first pane (with one terminal)
             * was already created by workspace_add. */
            if (json_object_has_member(ws_obj, "panes")) {
                gboolean restored_layout = FALSE;

                if (!strip_mode_active && json_object_has_member(ws_obj, "layout")) {
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

                if (strip_mode_active) {
                    while (ws->pane_notebooks &&
                           ws->pane_notebooks->len < n_panes) {
                        if (!workspace_split_current_for_layout(
                                ws, GTK_ORIENTATION_HORIZONTAL, ghostty_app))
                            break;
                    }
                }

                if ((restored_layout || strip_mode_active) && n_panes > 0)
                    session_assign_workspace_pane_ids_from_saved_order(
                        ws, panes_arr);

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

                    if (strip_mode_active) {
                        if (pi < ws->pane_notebooks->len)
                            nb = g_ptr_array_index(ws->pane_notebooks, pi);
                    } else if (restored_layout) {
                        if (pi < ws->pane_notebooks->len)
                            nb = g_ptr_array_index(ws->pane_notebooks, pi);
                    } else if (saved_pane_id[0]) {
                        nb = workspace_get_pane_by_id(ws, saved_pane_id);
                    } else if (pi > 0) {
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
                            /* Replace the bootstrap tab unconditionally so
                             * session restore never keeps the temporary
                             * terminal created before replay. */
                            {
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
                                    ws, nb, ghostty_app,
                                    first_cwd[0] ? first_cwd : NULL);
                                if (pi == 0 && first_cwd[0]) {
                                    snprintf(ws->cwd, sizeof(ws->cwd), "%s",
                                             first_cwd);
                                }
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

                if (ws->cwd[0])
                    workspace_detect_git(ws);

                if (strip_mode_active)
                    session_restore_workspace_strip_state(ws, ws_obj);
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
    g_timeout_add(500, session_finish_restore_cb, NULL);

    /* CWD is now set via ghostty_terminal_new(cwd) — no cd hack needed */
}

void
session_restore(GtkWindow *window, GtkWidget *browser_notebook,
                GtkWidget *terminal_stack, GtkWidget *workspace_list,
                ghostty_app_t ghostty_app,
                SessionAddBrowserTabFunc add_browser_tab_func)
{
    session_restore_for_instance(session_current_instance_id(), window,
                                 browser_notebook, terminal_stack,
                                 workspace_list, ghostty_app,
                                 add_browser_tab_func);
}
