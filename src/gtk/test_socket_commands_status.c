#include "socket_commands.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "app_actions.h"
#include "app_state.h"
#include "ghostty_terminal.h"
#include "notifications.h"
#include "shortcuts.h"
#include "socket_server.h"
#include "terminal_routing.h"
#include "workspace.h"
#include "workspace_strip.h"

GPtrArray *workspaces = NULL;
int current_workspace = 0;
GtkWidget *g_terminal_stack = NULL;
GtkWidget *g_workspace_list = NULL;

typedef struct {
    int count;
    Workspace *last_workspace;
    workspace_status_entry last_entry;
} NotifyCapture;

static NotifyCapture g_notify_capture = {0};
static int g_session_queue_save_calls = 0;
static int g_workspace_import_calls = 0;
static int g_workspace_import_next_index = 0;
static gboolean g_workspace_import_should_succeed = TRUE;
static char *g_workspace_import_last_payload = NULL;
static char *g_workspace_import_error = NULL;
static int g_workspace_move_calls = 0;
static int g_workspace_move_last_source = -1;
static int g_workspace_move_next_target_index = -1;
static gboolean g_workspace_move_should_succeed = TRUE;
static char g_workspace_move_last_target[96] = {0};
static char *g_workspace_move_error = NULL;

static void
workspace_status_entry_free(gpointer data)
{
    g_free(data);
}

static void
workspace_status_entry_normalize(workspace_status_entry *dest,
                                 const workspace_status_entry *src)
{
    const char *provider;
    const char *kind;

    memset(dest, 0, sizeof(*dest));
    if (!src)
        return;

    provider = src->provider[0] ? src->provider : "agent";
    kind = src->kind[0] ? src->kind : "status";

    if (src->entry_id[0]) {
        g_strlcpy(dest->entry_id, src->entry_id, sizeof(dest->entry_id));
    } else {
        g_snprintf(dest->entry_id, sizeof(dest->entry_id), "%s:%s", provider,
                   kind);
    }

    g_strlcpy(dest->provider, provider, sizeof(dest->provider));
    g_strlcpy(dest->kind, kind, sizeof(dest->kind));
    g_strlcpy(dest->status, src->status, sizeof(dest->status));
    g_strlcpy(dest->summary, src->summary, sizeof(dest->summary));
    g_strlcpy(dest->detail, src->detail, sizeof(dest->detail));
    if (!dest->summary[0]) {
        if (dest->detail[0])
            g_strlcpy(dest->summary, dest->detail, sizeof(dest->summary));
        else if (dest->status[0])
            g_strlcpy(dest->summary, dest->status, sizeof(dest->summary));
        else
            g_strlcpy(dest->summary, kind, sizeof(dest->summary));
    }
    dest->updated_at_usec = src->updated_at_usec > 0 ? src->updated_at_usec
                                                      : g_get_real_time();
    dest->attention = src->attention;
}

static gint
workspace_status_entry_compare(gconstpointer a, gconstpointer b)
{
    const workspace_status_entry *ea =
        *(const workspace_status_entry *const *)a;
    const workspace_status_entry *eb =
        *(const workspace_status_entry *const *)b;

    if (!ea && !eb)
        return 0;
    if (!ea)
        return 1;
    if (!eb)
        return -1;

    if (ea->attention != eb->attention)
        return ea->attention ? -1 : 1;
    if (ea->updated_at_usec != eb->updated_at_usec)
        return (ea->updated_at_usec > eb->updated_at_usec) ? -1 : 1;
    return g_strcmp0(ea->entry_id, eb->entry_id);
}

static Workspace *
workspace_test_new(const char *name)
{
    Workspace *ws = g_new0(Workspace, 1);

    ws->layout_mode = WORKSPACE_LAYOUT_CLASSIC;
    g_strlcpy(ws->name, name ? name : "Workspace", sizeof(ws->name));
    ws->status_entries = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, workspace_status_entry_free);
    return ws;
}

static void
workspace_test_free(gpointer data)
{
    Workspace *ws = data;

    if (!ws)
        return;
    if (ws->status_entries)
        g_hash_table_unref(ws->status_entries);
    g_free(ws);
}

static void
workspace_test_reset_state(void)
{
    if (workspaces)
        g_ptr_array_unref(workspaces);

    workspaces = g_ptr_array_new_with_free_func(workspace_test_free);
    current_workspace = 0;
    g_notify_capture.count = 0;
    g_notify_capture.last_workspace = NULL;
    memset(&g_notify_capture.last_entry, 0, sizeof(g_notify_capture.last_entry));
    g_session_queue_save_calls = 0;
    g_workspace_import_calls = 0;
    g_workspace_import_next_index = 0;
    g_workspace_import_should_succeed = TRUE;
    g_clear_pointer(&g_workspace_import_last_payload, g_free);
    g_clear_pointer(&g_workspace_import_error, g_free);
    g_workspace_move_calls = 0;
    g_workspace_move_last_source = -1;
    g_workspace_move_next_target_index = -1;
    g_workspace_move_should_succeed = TRUE;
    g_workspace_move_last_target[0] = '\0';
    g_clear_pointer(&g_workspace_move_error, g_free);

    app_state_set_instance_id("status-test-instance");
}

static Workspace *
workspace_test_add(const char *name)
{
    Workspace *ws;

    g_assert_nonnull(workspaces);
    ws = workspace_test_new(name);
    g_ptr_array_add(workspaces, ws);
    return ws;
}

static JsonNode *
invoke_socket_command(const char *command, JsonObject *msg)
{
    JsonBuilder *response = json_builder_new();
    JsonNode *root;

    json_builder_begin_object(response);
    socket_commands_on_socket_command(command, msg, response, NULL);
    json_builder_end_object(response);
    root = json_builder_get_root(response);
    g_object_unref(response);
    return root;
}

static JsonObject *
json_node_get_object_or_fail(JsonNode *root)
{
    g_assert_nonnull(root);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
    return json_node_get_object(root);
}

static void
assert_status_and_message(JsonObject *obj,
                          const char *expected_status,
                          const char *expected_message)
{
    g_assert_cmpstr(json_object_get_string_member_with_default(obj, "status", ""),
                    ==, expected_status);
    if (expected_message) {
        g_assert_cmpstr(
            json_object_get_string_member_with_default(obj, "message", ""), ==,
            expected_message);
    }
}

/* ---- Stubs required by socket_commands.c ---- */

const ShortcutDef default_shortcuts[] = {
    { .action = NULL, .keyval = 0, .mods = 0, .label = NULL },
};

void
app_actions_open_url_in_preferred_target(const char *url)
{
    (void)url;
}

gboolean
app_actions_handle_for_socket(const char *action,
                              gboolean non_interactive,
                              const char **error_out)
{
    (void)action;
    (void)non_interactive;
    if (error_out)
        *error_out = "stub";
    return FALSE;
}

void
app_actions_request_app_quit_async(void)
{
}

GhosttyTerminal *
notebook_terminal_at(GtkNotebook *notebook, int page_num)
{
    (void)notebook;
    (void)page_num;
    return NULL;
}

const char *
ghostty_terminal_get_cwd(GhosttyTerminal *self)
{
    (void)self;
    return NULL;
}

ghostty_surface_t
ghostty_terminal_get_surface(GhosttyTerminal *self)
{
    (void)self;
    return NULL;
}

void
ghostty_surface_text(ghostty_surface_t surface, const char *text, uintptr_t len)
{
    (void)surface;
    (void)text;
    (void)len;
}

bool
ghostty_surface_read_text(ghostty_surface_t surface,
                          ghostty_selection_s sel,
                          ghostty_text_s *out)
{
    (void)surface;
    (void)sel;
    if (out)
        memset(out, 0, sizeof(*out));
    return false;
}

void
ghostty_surface_free_text(ghostty_surface_t surface, ghostty_text_s *text)
{
    (void)surface;
    (void)text;
}

void
notifications_publish_workspace_status(Workspace *ws,
                                       const workspace_status_entry *entry,
                                       gboolean allow_toast)
{
    (void)allow_toast;
    g_notify_capture.count++;
    g_notify_capture.last_workspace = ws;
    if (entry)
        g_notify_capture.last_entry = *entry;
    else
        memset(&g_notify_capture.last_entry, 0, sizeof(g_notify_capture.last_entry));
}

void
session_queue_save(void)
{
    g_session_queue_save_calls++;
}

gboolean
socket_server_route_command_to_instance(const char *instance_id,
                                        JsonObject *msg,
                                        JsonBuilder *response,
                                        GError **error)
{
    (void)instance_id;
    (void)msg;
    (void)response;
    if (error) {
        g_set_error_literal(error,
                            g_quark_from_static_string("test-socket-route"), 1,
                            "routing not available in test");
    }
    return FALSE;
}

void
terminal_routing_register_scope(const char *terminal_id,
                                pid_t session_id,
                                const char *tty_name,
                                const char *tty_path)
{
    (void)terminal_id;
    (void)session_id;
    (void)tty_name;
    (void)tty_path;
}

void
workspace_add(GtkWidget *terminal_stack,
              GtkWidget *workspace_list,
              ghostty_app_t app)
{
    (void)terminal_stack;
    (void)workspace_list;
    (void)app;
    if (!workspaces)
        workspaces = g_ptr_array_new_with_free_func(workspace_test_free);
    g_ptr_array_add(workspaces, workspace_test_new("new"));
}

void
workspace_add_terminal_to_focused(Workspace *ws, ghostty_app_t app)
{
    (void)ws;
    (void)app;
}

void
workspace_close_pane(Workspace *ws, GtkNotebook *pane)
{
    (void)ws;
    (void)pane;
}

gboolean
workspace_equalize_splits(Workspace *ws, const char *orientation_name)
{
    (void)ws;
    (void)orientation_name;
    return FALSE;
}

gboolean
workspace_focus_pane(Workspace *ws, GtkNotebook *pane)
{
    (void)ws;
    (void)pane;
    return FALSE;
}

Workspace *
workspace_get_current(void)
{
    if (!workspaces || current_workspace < 0 ||
        current_workspace >= (int)workspaces->len) {
        return NULL;
    }

    return g_ptr_array_index(workspaces, (guint)current_workspace);
}

GtkNotebook *
workspace_get_focused_pane(Workspace *ws)
{
    (void)ws;
    return NULL;
}

WorkspaceLayoutMode
workspace_get_layout_mode(Workspace *ws)
{
    return ws ? ws->layout_mode : WORKSPACE_LAYOUT_CLASSIC;
}

GtkNotebook *
workspace_strip_column_focused_notebook(WorkspaceColumn *col)
{
    (void)col;
    return NULL;
}

GtkNotebook *
workspace_get_pane_by_id(Workspace *ws, const char *pane_id)
{
    (void)ws;
    (void)pane_id;
    return NULL;
}

GtkNotebook *
workspace_get_pane_by_index(Workspace *ws, int pane_idx)
{
    (void)ws;
    (void)pane_idx;
    return NULL;
}

const char *
workspace_get_pane_id(GtkNotebook *pane)
{
    (void)pane;
    return "";
}

int
workspace_get_pane_index(Workspace *ws, GtkNotebook *pane)
{
    (void)ws;
    (void)pane;
    return -1;
}

void
workspace_set_status_entry(Workspace *ws, const workspace_status_entry *entry)
{
    workspace_status_entry *owned;

    if (!ws || !entry)
        return;

    if (!ws->status_entries) {
        ws->status_entries = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, workspace_status_entry_free);
    }

    owned = g_new0(workspace_status_entry, 1);
    workspace_status_entry_normalize(owned, entry);
    if (!owned->entry_id[0]) {
        g_free(owned);
        return;
    }

    g_hash_table_replace(ws->status_entries, g_strdup(owned->entry_id), owned);
}

void
workspace_clear_status_entry(Workspace *ws, const char *entry_id)
{
    if (!ws || !ws->status_entries)
        return;

    if (!entry_id || !entry_id[0])
        g_hash_table_remove_all(ws->status_entries);
    else
        g_hash_table_remove(ws->status_entries, entry_id);
}

GPtrArray *
workspace_get_sorted_status_entries(Workspace *ws)
{
    GPtrArray *entries = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (!ws || !ws->status_entries)
        return entries;

    g_hash_table_iter_init(&iter, ws->status_entries);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        (void)key;
        if (value)
            g_ptr_array_add(entries, value);
    }

    if (entries->len > 1)
        g_ptr_array_sort(entries, workspace_status_entry_compare);

    return entries;
}

gboolean
workspace_move_tab(int src_ws_idx,
                   int src_pane_idx,
                   int src_tab_idx,
                   int dest_ws_idx,
                   int dest_pane_idx)
{
    (void)src_ws_idx;
    (void)src_pane_idx;
    (void)src_tab_idx;
    (void)dest_ws_idx;
    (void)dest_pane_idx;
    return FALSE;
}

Workspace *
workspace_detach_from_instance(int index)
{
    (void)index;
    return NULL;
}

gboolean
workspace_attach_to_instance(Workspace *ws, int target_index)
{
    (void)ws;
    (void)target_index;
    return FALSE;
}

gboolean
workspace_import_from_payload(const char *payload,
                              ghostty_app_t app,
                              int *out_workspace_index,
                              char **error_out)
{
    (void)app;
    g_workspace_import_calls++;
    g_free(g_workspace_import_last_payload);
    g_workspace_import_last_payload = g_strdup(payload ? payload : "");

    if (out_workspace_index)
        *out_workspace_index = g_workspace_import_next_index;
    if (error_out)
        *error_out = NULL;

    if (g_workspace_import_should_succeed)
        return TRUE;

    if (error_out) {
        *error_out = g_strdup(
            g_workspace_import_error ? g_workspace_import_error : "import failed");
    }
    return FALSE;
}

gboolean
workspace_move_to_instance(int source_workspace_index,
                           const char *target_instance_id,
                           int *out_target_workspace_index,
                           char **error_out)
{
    g_workspace_move_calls++;
    g_workspace_move_last_source = source_workspace_index;
    g_strlcpy(g_workspace_move_last_target,
              target_instance_id ? target_instance_id : "",
              sizeof(g_workspace_move_last_target));

    if (out_target_workspace_index)
        *out_target_workspace_index = g_workspace_move_next_target_index;
    if (error_out)
        *error_out = NULL;

    if (g_workspace_move_should_succeed)
        return TRUE;

    if (error_out) {
        *error_out =
            g_strdup(g_workspace_move_error ? g_workspace_move_error
                                            : "move failed");
    }
    return FALSE;
}

gboolean
workspace_rebuild_for_layout_mode(Workspace *ws, WorkspaceLayoutMode mode)
{
    if (!ws)
        return FALSE;

    ws->layout_mode = mode;
    return TRUE;
}

int
workspace_apply_layout_mode_to_all(WorkspaceLayoutMode mode)
{
    int updated = 0;

    if (!workspaces)
        return 0;

    for (guint i = 0; i < workspaces->len; i++) {
        Workspace *ws = g_ptr_array_index(workspaces, i);
        if (!ws || ws->layout_mode == mode)
            continue;
        ws->layout_mode = mode;
        updated++;
    }

    return updated;
}

void
workspace_refresh_sidebar_label(Workspace *ws)
{
    (void)ws;
}

gboolean
workspace_resize_pane_percent(Workspace *ws,
                              GtkNotebook *pane,
                              char axis,
                              double percent)
{
    (void)ws;
    (void)pane;
    (void)axis;
    (void)percent;
    return FALSE;
}

gboolean
workspace_select_tab(int ws_idx, int pane_idx, int tab_idx)
{
    (void)ws_idx;
    (void)pane_idx;
    (void)tab_idx;
    return FALSE;
}

GtkNotebook *
workspace_split_pane_target(Workspace *ws,
                            GtkNotebook *pane,
                            GtkOrientation orientation,
                            ghostty_app_t app)
{
    (void)ws;
    (void)pane;
    (void)orientation;
    (void)app;
    return NULL;
}

void
workspace_start_tab_rename(Workspace *ws)
{
    (void)ws;
}

void
workspace_start_workspace_rename(Workspace *ws)
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

/* ---- Tests ---- */

static void
test_workspace_status_set_list_clear_roundtrip(void)
{
    Workspace *ws;
    JsonObject *msg;
    JsonNode *root;
    JsonObject *obj;
    JsonArray *entries;

    workspace_test_reset_state();
    ws = workspace_test_add("Phase 7");

    msg = json_object_new();
    json_object_set_string_member(msg, "entryId", "agent.main");
    json_object_set_string_member(msg, "provider", "codex");
    json_object_set_string_member(msg, "kind", "session");
    json_object_set_string_member(msg, "state", "running");
    json_object_set_string_member(msg, "summary", "indexing");
    json_object_set_string_member(msg, "detail", "indexing repository");
    json_object_set_boolean_member(msg, "attention", FALSE);
    json_object_set_int_member(msg, "updatedAtUsec", 200);
    json_object_set_int_member(msg, "workspace", 0);
    root = invoke_socket_command("workspace.status.set", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    g_assert_cmpint(json_object_get_int_member_with_default(obj, "workspace", -1),
                    ==, 0);
    g_assert_cmpstr(
        json_object_get_string_member_with_default(obj, "entryId", ""), ==,
        "agent.main");
    json_node_free(root);
    json_object_unref(msg);

    msg = json_object_new();
    json_object_set_string_member(msg, "entryId", "agent.review");
    json_object_set_string_member(msg, "provider", "claude");
    json_object_set_string_member(msg, "kind", "review");
    json_object_set_string_member(msg, "state", "blocked");
    json_object_set_string_member(msg, "summary", "waiting for input");
    json_object_set_boolean_member(msg, "attention", TRUE);
    json_object_set_int_member(msg, "updatedAtUsec", 100);
    json_object_set_int_member(msg, "workspace", 0);
    root = invoke_socket_command("workspace.status.set", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    json_node_free(root);
    json_object_unref(msg);

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 0);
    root = invoke_socket_command("workspace.status.list", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    entries = json_object_get_array_member(obj, "entries");
    g_assert_nonnull(entries);
    g_assert_cmpuint(json_array_get_length(entries), ==, 2);

    {
        JsonObject *first = json_array_get_object_element(entries, 0);
        JsonObject *second = json_array_get_object_element(entries, 1);

        g_assert_cmpstr(
            json_object_get_string_member_with_default(first, "entryId", ""),
            ==, "agent.review");
        g_assert_true(
            json_object_get_boolean_member_with_default(first, "attention", FALSE));

        g_assert_cmpstr(
            json_object_get_string_member_with_default(second, "entryId", ""),
            ==, "agent.main");
        g_assert_cmpstr(
            json_object_get_string_member_with_default(second, "provider", ""),
            ==, "codex");
        g_assert_cmpstr(
            json_object_get_string_member_with_default(second, "kind", ""), ==,
            "session");
        g_assert_cmpstr(
            json_object_get_string_member_with_default(second, "status", ""),
            ==, "running");
        g_assert_cmpstr(
            json_object_get_string_member_with_default(second, "summary", ""),
            ==, "indexing");
        g_assert_cmpstr(
            json_object_get_string_member_with_default(second, "detail", ""), ==,
            "indexing repository");
    }
    json_node_free(root);
    json_object_unref(msg);

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 0);
    json_object_set_string_member(msg, "entryId", "agent.review");
    root = invoke_socket_command("workspace.status.clear", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    json_node_free(root);
    json_object_unref(msg);

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 0);
    root = invoke_socket_command("workspace.status.list", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    entries = json_object_get_array_member(obj, "entries");
    g_assert_nonnull(entries);
    g_assert_cmpuint(json_array_get_length(entries), ==, 1);
    g_assert_cmpstr(
        json_object_get_string_member_with_default(
            json_array_get_object_element(entries, 0), "entryId", ""),
        ==, "agent.main");
    json_node_free(root);
    json_object_unref(msg);

    g_assert_cmpint(g_notify_capture.count, ==, 0);
    g_assert_cmpint(g_session_queue_save_calls, ==, 0);
    g_assert_nonnull(ws);
}

static void
test_workspace_status_notify_and_detail_only(void)
{
    Workspace *ws;
    JsonObject *msg;
    JsonNode *root;
    JsonObject *obj;
    JsonArray *entries;

    workspace_test_reset_state();
    ws = workspace_test_add("Notify");

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 0);
    json_object_set_string_member(msg, "entryId", "agent.detail");
    json_object_set_string_member(msg, "provider", "codex");
    json_object_set_string_member(msg, "kind", "session");
    json_object_set_string_member(msg, "detail", "generating patch");
    json_object_set_boolean_member(msg, "notify", TRUE);
    root = invoke_socket_command("workspace.status.set", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    json_node_free(root);
    json_object_unref(msg);

    g_assert_cmpint(g_notify_capture.count, ==, 1);
    g_assert_true(g_notify_capture.last_workspace == ws);
    g_assert_cmpstr(g_notify_capture.last_entry.entry_id, ==, "agent.detail");
    g_assert_cmpstr(g_notify_capture.last_entry.detail, ==, "generating patch");

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 0);
    root = invoke_socket_command("workspace.status.list", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    entries = json_object_get_array_member(obj, "entries");
    g_assert_nonnull(entries);
    g_assert_cmpuint(json_array_get_length(entries), ==, 1);
    g_assert_cmpstr(
        json_object_get_string_member_with_default(
            json_array_get_object_element(entries, 0), "summary", ""),
        ==, "generating patch");
    json_node_free(root);
    json_object_unref(msg);

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 0);
    json_object_set_string_member(msg, "entryId", "agent.detail");
    json_object_set_string_member(msg, "detail", "still running");
    json_object_set_boolean_member(msg, "notify", FALSE);
    root = invoke_socket_command("workspace.status.set", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    json_node_free(root);
    json_object_unref(msg);

    g_assert_cmpint(g_notify_capture.count, ==, 1);
    g_assert_cmpint(g_session_queue_save_calls, ==, 0);
}

static void
test_workspace_status_invalid_workspace_errors(void)
{
    JsonObject *msg;
    JsonNode *root;
    JsonObject *obj;

    workspace_test_reset_state();
    workspace_test_add("Only");

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 99);
    json_object_set_string_member(msg, "entryId", "bad");
    json_object_set_string_member(msg, "summary", "x");
    root = invoke_socket_command("workspace.status.set", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "error", "invalid workspace index");
    json_node_free(root);
    json_object_unref(msg);

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 99);
    json_object_set_string_member(msg, "entryId", "bad");
    root = invoke_socket_command("workspace.status.clear", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "error", "invalid workspace index");
    json_node_free(root);
    json_object_unref(msg);

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 99);
    root = invoke_socket_command("workspace.status.list", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "error", "invalid workspace index");
    json_node_free(root);
    json_object_unref(msg);

    g_assert_cmpint(g_notify_capture.count, ==, 0);
    g_assert_cmpint(g_session_queue_save_calls, ==, 0);
}

static void
test_workspace_set_layout_without_target_updates_all_workspaces(void)
{
    JsonObject *msg;
    JsonNode *root;
    JsonObject *obj;
    Workspace *ws0;
    Workspace *ws1;
    Workspace *ws2;

    workspace_test_reset_state();
    ws0 = workspace_test_add("one");
    ws1 = workspace_test_add("two");
    ws2 = workspace_test_add("three");
    ws2->layout_mode = WORKSPACE_LAYOUT_STRIP;

    msg = json_object_new();
    json_object_set_string_member(msg, "layout", "strip");
    root = invoke_socket_command("workspace.set_layout", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    g_assert_cmpstr(json_object_get_string_member_with_default(obj, "layout", ""),
                    ==, "strip");
    g_assert_cmpstr(json_object_get_string_member_with_default(obj, "scope", ""),
                    ==, "all");
    g_assert_cmpint(
        json_object_get_int_member_with_default(obj, "workspaceCount", -1), ==,
        3);
    g_assert_cmpint(
        json_object_get_int_member_with_default(obj, "updatedCount", -1), ==,
        2);
    json_node_free(root);
    json_object_unref(msg);

    g_assert_cmpint(ws0->layout_mode, ==, WORKSPACE_LAYOUT_STRIP);
    g_assert_cmpint(ws1->layout_mode, ==, WORKSPACE_LAYOUT_STRIP);
    g_assert_cmpint(ws2->layout_mode, ==, WORKSPACE_LAYOUT_STRIP);
}

static void
test_workspace_set_layout_target_keeps_workspace_scope(void)
{
    JsonObject *msg;
    JsonNode *root;
    JsonObject *obj;
    Workspace *ws0;
    Workspace *ws1;
    Workspace *ws2;

    workspace_test_reset_state();
    ws0 = workspace_test_add("one");
    ws1 = workspace_test_add("two");
    ws2 = workspace_test_add("three");

    msg = json_object_new();
    json_object_set_string_member(msg, "layout", "strip");
    json_object_set_int_member(msg, "workspace", 1);
    root = invoke_socket_command("workspace.set_layout", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    g_assert_cmpstr(json_object_get_string_member_with_default(obj, "layout", ""),
                    ==, "strip");
    g_assert_cmpstr(json_object_get_string_member_with_default(obj, "scope", ""),
                    ==, "workspace");
    g_assert_cmpint(json_object_get_int_member_with_default(obj, "workspace", -1),
                    ==, 1);
    json_node_free(root);
    json_object_unref(msg);

    g_assert_cmpint(ws0->layout_mode, ==, WORKSPACE_LAYOUT_CLASSIC);
    g_assert_cmpint(ws1->layout_mode, ==, WORKSPACE_LAYOUT_STRIP);
    g_assert_cmpint(ws2->layout_mode, ==, WORKSPACE_LAYOUT_CLASSIC);
}

static void
test_workspace_import_command_uses_backend(void)
{
    JsonObject *msg;
    JsonNode *root;
    JsonObject *obj;

    workspace_test_reset_state();
    workspace_test_add("one");
    g_workspace_import_should_succeed = TRUE;
    g_workspace_import_next_index = 4;

    msg = json_object_new();
    json_object_set_string_member(msg, "workspacePayload",
                                  "{\"name\":\"Moved\"}");
    root = invoke_socket_command("workspace.import", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    g_assert_cmpint(json_object_get_int_member_with_default(obj, "index", -1),
                    ==, 4);
    g_assert_cmpint(g_workspace_import_calls, ==, 1);
    g_assert_cmpstr(g_workspace_import_last_payload, ==,
                    "{\"name\":\"Moved\"}");

    json_node_free(root);
    json_object_unref(msg);
}

static void
test_workspace_move_to_instance_command_uses_backend(void)
{
    JsonObject *msg;
    JsonNode *root;
    JsonObject *obj;

    workspace_test_reset_state();
    workspace_test_add("one");
    workspace_test_add("two");
    current_workspace = 1;
    g_workspace_move_should_succeed = TRUE;
    g_workspace_move_next_target_index = 9;

    msg = json_object_new();
    json_object_set_int_member(msg, "workspace", 0);
    json_object_set_string_member(msg, "targetInstanceId", "other-instance");
    root = invoke_socket_command("workspace.move_to_instance", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "ok", NULL);
    g_assert_cmpint(json_object_get_int_member_with_default(obj, "workspace", -1),
                    ==, 0);
    g_assert_cmpstr(
        json_object_get_string_member_with_default(obj, "targetInstanceId", ""),
        ==, "other-instance");
    g_assert_cmpint(
        json_object_get_int_member_with_default(obj, "targetWorkspace", -1), ==,
        9);
    g_assert_cmpint(g_workspace_move_calls, ==, 1);
    g_assert_cmpint(g_workspace_move_last_source, ==, 0);
    g_assert_cmpstr(g_workspace_move_last_target, ==, "other-instance");

    json_node_free(root);
    json_object_unref(msg);
}

static void
test_workspace_move_to_instance_command_reports_backend_error(void)
{
    JsonObject *msg;
    JsonNode *root;
    JsonObject *obj;

    workspace_test_reset_state();
    workspace_test_add("one");
    g_workspace_move_should_succeed = FALSE;
    g_free(g_workspace_move_error);
    g_workspace_move_error = g_strdup("target unavailable");

    msg = json_object_new();
    json_object_set_string_member(msg, "targetInstanceId", "other-instance");
    root = invoke_socket_command("workspace.move_to_instance", msg);
    obj = json_node_get_object_or_fail(root);
    assert_status_and_message(obj, "error", "target unavailable");
    g_assert_cmpint(g_workspace_move_calls, ==, 1);
    g_assert_cmpint(g_workspace_move_last_source, ==, 0);

    json_node_free(root);
    json_object_unref(msg);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/socket-commands/status/set-list-clear-roundtrip",
                    test_workspace_status_set_list_clear_roundtrip);
    g_test_add_func("/socket-commands/status/notify-and-detail-only",
                    test_workspace_status_notify_and_detail_only);
    g_test_add_func("/socket-commands/status/invalid-workspace-errors",
                    test_workspace_status_invalid_workspace_errors);
    g_test_add_func("/socket-commands/workspace/set-layout-all",
                    test_workspace_set_layout_without_target_updates_all_workspaces);
    g_test_add_func("/socket-commands/workspace/set-layout-targeted",
                    test_workspace_set_layout_target_keeps_workspace_scope);
    g_test_add_func("/socket-commands/workspace/import",
                    test_workspace_import_command_uses_backend);
    g_test_add_func("/socket-commands/workspace/move-to-instance",
                    test_workspace_move_to_instance_command_uses_backend);
    g_test_add_func("/socket-commands/workspace/move-to-instance-error",
                    test_workspace_move_to_instance_command_reports_backend_error);

    return g_test_run();
}
