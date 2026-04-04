#include "prettymux_agent_cli.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <io.h>
#include <process.h>
#define prettymux_close _close
#define prettymux_execv _execv
#else
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define prettymux_close close
#define prettymux_execv execv
#endif

typedef struct {
    int workspace;
    char *leader_pane;
    char *last_column_pane;
    char *layout;
} TmuxCompatState;

static void tmux_state_clear(TmuxCompatState *state);
static gboolean run_claude_teams(int argc, char **argv, int *exit_code);
static gboolean run_tmux_compat(int argc, char **argv, int *exit_code);
static char *replace_token(const char *input, const char *token,
                           const char *value);
static gboolean is_claude_wrapper_candidate(const char *path);
static char *create_node_options_restore_module(GError **error);
static char *merge_node_options(const char *existing,
                                const char *restore_module_path);

gboolean
prettymux_agent_cli_maybe_run(int argc, char **argv, int *exit_code)
{
    if (exit_code)
        *exit_code = 0;

    if (argc < 2 || !argv[1])
        return FALSE;

    if (strcmp(argv[1], "claude-teams") == 0)
        return run_claude_teams(argc - 2, argv + 2, exit_code);

    if (strcmp(argv[1], "__tmux-compat") == 0)
        return run_tmux_compat(argc - 2, argv + 2, exit_code);

    return FALSE;
}

static void
set_error_literal(GError **error, const char *message)
{
    g_set_error_literal(error, g_quark_from_static_string("prettymux-agent-cli"),
                        1, message);
}

static char *
json_escape_string(const char *input)
{
    GString *out;

    if (!input)
        return g_strdup("");

    out = g_string_new("");
    for (const unsigned char *c = (const unsigned char *)input; *c; c++) {
        switch (*c) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if (*c < 0x20)
                g_string_append_printf(out, "\\u%04x", *c);
            else
                g_string_append_c(out, (char)*c);
        }
    }

    return g_string_free(out, FALSE);
}

#ifndef G_OS_WIN32
static gboolean
socket_is_connectable(const char *path)
{
    int fd;
    struct sockaddr_un addr;
    gboolean ok = FALSE;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return FALSE;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

static char *
find_socket_path(GError **error)
{
    const char *env = g_getenv("PRETTYMUX_SOCKET");
    DIR *dir;
    struct dirent *ent;

    if (env && env[0] && socket_is_connectable(env))
        return g_strdup(env);

    dir = opendir("/tmp");
    if (!dir) {
        g_set_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                    errno, "Could not open /tmp");
        return NULL;
    }

    while ((ent = readdir(dir)) != NULL) {
        char candidate[256];

        if (strncmp(ent->d_name, "prettymux-", 10) != 0 ||
            strstr(ent->d_name, ".sock") == NULL)
            continue;

        g_snprintf(candidate, sizeof(candidate), "/tmp/%s", ent->d_name);
        if (socket_is_connectable(candidate)) {
            closedir(dir);
            return g_strdup(candidate);
        }
    }

    closedir(dir);
    set_error_literal(error,
                      "No running PrettyMux instance found. Run this inside PrettyMux or set PRETTYMUX_SOCKET.");
    return NULL;
}

static char *
send_json_message(const char *json_msg, GError **error)
{
    g_autofree char *socket_path = NULL;
    int fd;
    struct sockaddr_un addr;
    GString *response;
    char buffer[8192];
    ssize_t read_bytes;

    socket_path = find_socket_path(error);
    if (!socket_path)
        return NULL;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_set_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                    errno, "socket() failed");
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        g_set_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                    errno, "connect() failed");
        close(fd);
        return NULL;
    }

    if (write(fd, json_msg, strlen(json_msg)) < 0) {
        g_set_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                    errno, "write() failed");
        close(fd);
        return NULL;
    }

    shutdown(fd, SHUT_WR);
    response = g_string_new("");
    while ((read_bytes = read(fd, buffer, sizeof(buffer))) > 0)
        g_string_append_len(response, buffer, read_bytes);
    close(fd);

    if (read_bytes < 0) {
        g_set_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                    errno, "read() failed");
        g_string_free(response, TRUE);
        return NULL;
    }

    return g_string_free(response, FALSE);
}
#else
static char *
send_json_message(const char *json_msg, GError **error)
{
    (void)json_msg;
    set_error_literal(error, "PrettyMux claude-teams is not supported on Windows.");
    return NULL;
}
#endif

static JsonParser *
parse_response_parser(const char *payload, GError **error)
{
    JsonParser *parser = json_parser_new();

    if (!json_parser_load_from_data(parser, payload ? payload : "", -1, error)) {
        g_object_unref(parser);
        return NULL;
    }

    if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_object_unref(parser);
        set_error_literal(error, "PrettyMux returned a non-object response.");
        return NULL;
    }

    return parser;
}

static JsonParser *
ipc_request(const char *json_msg, GError **error)
{
    g_autofree char *payload = send_json_message(json_msg, error);
    JsonParser *parser;
    JsonObject *root;
    const char *status;

    if (!payload)
        return NULL;

    parser = parse_response_parser(payload, error);
    if (!parser)
        return NULL;

    root = json_node_get_object(json_parser_get_root(parser));
    status = json_object_get_string_member_with_default(root, "status", "");
    if (g_strcmp0(status, "ok") != 0) {
        g_autofree char *message = g_strdup(
            json_object_get_string_member_with_default(root, "message",
                                                       "PrettyMux command failed"));
        g_object_unref(parser);
        set_error_literal(error, message);
        return NULL;
    }

    return parser;
}

static JsonParser *
ipc_workspace_current(GError **error)
{
    return ipc_request("{\"command\":\"workspace.current\"}", error);
}

static JsonParser *
ipc_pane_list(int workspace, GError **error)
{
    char *msg = g_strdup_printf("{\"command\":\"pane.list\",\"workspace\":%d}",
                                workspace);
    JsonParser *parser = ipc_request(msg, error);
    g_free(msg);
    return parser;
}

static char *
ipc_resolve_leader_pane(int workspace, const char *preferred, GError **error)
{
    JsonParser *parser;
    JsonObject *root;
    JsonArray *panes;
    g_autofree char *fallback = NULL;

    parser = ipc_pane_list(workspace, error);
    if (!parser)
        return NULL;

    root = json_node_get_object(json_parser_get_root(parser));
    panes = json_object_get_array_member(root, "panes");
    if (panes) {
        for (guint i = 0; i < json_array_get_length(panes); i++) {
            JsonObject *pane = json_array_get_object_element(panes, i);
            const char *pane_id;

            if (!pane)
                continue;
            pane_id =
                json_object_get_string_member_with_default(pane, "id", "");
            if (!pane_id[0])
                continue;
            if (!fallback)
                fallback = g_strdup(pane_id);
            if (preferred && preferred[0] && g_strcmp0(preferred, pane_id) == 0) {
                char *match = g_strdup(pane_id);
                g_object_unref(parser);
                return match;
            }
        }
    }

    g_object_unref(parser);
    return g_steal_pointer(&fallback);
}

static gboolean
ipc_pane_focus(int workspace, const char *pane_id, GError **error)
{
    g_autofree char *escaped = json_escape_string(pane_id);
    g_autofree char *msg = g_strdup_printf(
        "{\"command\":\"pane.focus\",\"workspace\":%d,\"paneId\":\"%s\"}",
        workspace, escaped);
    JsonParser *parser = ipc_request(msg, error);
    if (!parser)
        return FALSE;
    g_object_unref(parser);
    return TRUE;
}

static JsonParser *
ipc_pane_split(int workspace, const char *pane_id, const char *direction,
               GError **error)
{
    g_autofree char *escaped_pane = json_escape_string(pane_id);
    g_autofree char *escaped_dir = json_escape_string(direction);
    g_autofree char *msg = g_strdup_printf(
        "{\"command\":\"pane.split\",\"workspace\":%d,\"paneId\":\"%s\",\"direction\":\"%s\"}",
        workspace, escaped_pane, escaped_dir);
    return ipc_request(msg, error);
}

static gboolean
ipc_pane_close(int workspace, const char *pane_id, GError **error)
{
    g_autofree char *escaped = json_escape_string(pane_id);
    g_autofree char *msg = g_strdup_printf(
        "{\"command\":\"pane.close\",\"workspace\":%d,\"paneId\":\"%s\"}",
        workspace, escaped);
    JsonParser *parser = ipc_request(msg, error);
    if (!parser)
        return FALSE;
    g_object_unref(parser);
    return TRUE;
}

static gboolean
ipc_equalize(int workspace, const char *orientation, GError **error)
{
    g_autofree char *escaped = json_escape_string(orientation ? orientation : "");
    g_autofree char *msg = g_strdup_printf(
        "{\"command\":\"workspace.equalize_splits\",\"workspace\":%d,\"orientation\":\"%s\"}",
        workspace, escaped);
    JsonParser *parser = ipc_request(msg, error);
    if (!parser)
        return FALSE;
    g_object_unref(parser);
    return TRUE;
}

static gboolean
ipc_resize_percent(int workspace, const char *pane_id, char axis,
                   double percent, GError **error)
{
    g_autofree char *escaped = json_escape_string(pane_id);
    g_autofree char *msg = g_strdup_printf(
        "{\"command\":\"pane.resize_percent\",\"workspace\":%d,\"paneId\":\"%s\",\"axis\":\"%c\",\"percent\":%.2f}",
        workspace, escaped, axis, percent);
    JsonParser *parser = ipc_request(msg, error);
    if (!parser)
        return FALSE;
    g_object_unref(parser);
    return TRUE;
}

static char *
find_executable_in_path(const char *name, const char *path_env,
                        const char *skip_dir)
{
    g_auto(GStrv) entries = NULL;

    if (!path_env || !path_env[0])
        return NULL;

    entries = g_strsplit(path_env, G_SEARCHPATH_SEPARATOR_S, -1);
    for (guint i = 0; entries && entries[i]; i++) {
        g_autofree char *candidate = NULL;

        if (!entries[i][0])
            continue;
        if (skip_dir && g_strcmp0(entries[i], skip_dir) == 0)
            continue;

        candidate = g_build_filename(entries[i], name, NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE) &&
            !is_claude_wrapper_candidate(candidate))
            return g_strdup(candidate);
    }

    return NULL;
}

static gboolean
is_claude_wrapper_candidate(const char *path)
{
    g_autofree char *contents = NULL;
    gsize len = 0;

    if (!path || !path[0])
        return FALSE;
    if (!g_file_get_contents(path, &contents, &len, NULL) || !contents)
        return FALSE;
    if (!g_str_has_prefix(contents, "#!"))
        return FALSE;

    return strstr(contents, "CLAUDECODE") != NULL ||
           strstr(contents, "__tmux-compat") != NULL ||
           strstr(contents, "CMUX_CLAUDE_HOOKS") != NULL ||
           strstr(contents, "PRETTYMUX_CLAUDE_TEAMS_BIN") != NULL;
}

static char *
current_executable_path(void)
{
#ifdef G_OS_WIN32
    return NULL;
#else
    return g_file_read_link("/proc/self/exe", NULL);
#endif
}

static gboolean
write_text_if_changed(const char *path, const char *content, GError **error)
{
    g_autofree char *existing = NULL;
    gsize existing_len = 0;

    if (g_file_get_contents(path, &existing, &existing_len, NULL) &&
        g_strcmp0(existing, content) == 0)
        return TRUE;

    return g_file_set_contents(path, content, -1, error);
}

static char *
create_node_options_restore_module(GError **error)
{
    static const char *module_contents =
        "const hadOriginalNodeOptions =\n"
        "  process.env.CMUX_ORIGINAL_NODE_OPTIONS_PRESENT === \"1\";\n"
        "if (hadOriginalNodeOptions) {\n"
        "  process.env.NODE_OPTIONS = process.env.CMUX_ORIGINAL_NODE_OPTIONS ?? \"\";\n"
        "} else {\n"
        "  delete process.env.NODE_OPTIONS;\n"
        "}\n"
        "delete process.env.CMUX_ORIGINAL_NODE_OPTIONS;\n"
        "delete process.env.CMUX_ORIGINAL_NODE_OPTIONS_PRESENT;\n";
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;

    dir = g_build_filename(g_get_tmp_dir(), "prettymux-claude-node-options",
                           NULL);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                    errno, "Could not create NODE_OPTIONS restore module directory");
        return NULL;
    }

    path = g_build_filename(dir, "restore-node-options.cjs", NULL);
    if (!write_text_if_changed(path, module_contents, error))
        return NULL;

    return g_steal_pointer(&path);
}

static char *
merge_node_options(const char *existing, const char *restore_module_path)
{
    g_auto(GStrv) tokens = NULL;
    GPtrArray *filtered;
    GString *merged;
    gboolean skip_next = FALSE;

    if (!restore_module_path || !restore_module_path[0])
        return g_strdup(existing ? existing : "");

    filtered = g_ptr_array_new_with_free_func(g_free);
    if (existing && existing[0]) {
        tokens = g_strsplit(existing, " ", -1);
        for (guint i = 0; tokens && tokens[i]; i++) {
            const char *token = tokens[i];

            if (!token[0])
                continue;
            if (skip_next) {
                skip_next = FALSE;
                continue;
            }
            if (strcmp(token, "--max-old-space-size") == 0) {
                skip_next = TRUE;
                continue;
            }
            if (g_str_has_prefix(token, "--max-old-space-size="))
                continue;
            g_ptr_array_add(filtered, g_strdup(token));
        }
    }

    merged = g_string_new("");
    g_string_append_printf(merged, "--require=%s --max-old-space-size=4096",
                           restore_module_path);
    for (guint i = 0; i < filtered->len; i++) {
        const char *token = g_ptr_array_index(filtered, i);
        g_string_append_c(merged, ' ');
        g_string_append(merged, token);
    }
    g_ptr_array_free(filtered, TRUE);
    return g_string_free(merged, FALSE);
}

static char *
ensure_tmux_shim_dir(const char *exe_path, GError **error)
{
    g_autofree char *dir = g_build_filename(g_get_user_config_dir(),
                                            "prettymux",
                                            "claude-teams-bin", NULL);
    g_autofree char *tmux_path = g_build_filename(dir, "tmux", NULL);
    g_autofree char *script = NULL;

    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_set_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                    errno, "Could not create tmux shim directory");
        return NULL;
    }

    script = g_strdup_printf(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        "case \"${1:-}\" in\n"
        "  -V|-v) echo \"tmux 3.4\"; exit 0 ;;\n"
        "esac\n"
        "_pmux_log=\"${PRETTYMUX_TMUX_LOG:-}\"\n"
        "if [ -n \"$_pmux_log\" ]; then\n"
        "  echo \"[$(date +%%H:%%M:%%S)] tmux $*\" >> \"$_pmux_log\"\n"
        "fi\n"
        "_pmux_bin=\"${PRETTYMUX_CLAUDE_TEAMS_BIN:-%s}\"\n"
        "_pmux_out=$( \"$_pmux_bin\" __tmux-compat \"$@\" 2>&1 )\n"
        "_pmux_rc=$?\n"
        "if [ -n \"$_pmux_log\" ]; then\n"
        "  echo \"  -> rc=$_pmux_rc out=$(echo \"$_pmux_out\" | head -5)\" >> \"$_pmux_log\"\n"
        "fi\n"
        "[ -n \"$_pmux_out\" ] && printf '%%s\\n' \"$_pmux_out\"\n"
        "exit $_pmux_rc\n",
        exe_path ? exe_path : "prettymux");

    if (!write_text_if_changed(tmux_path, script, error))
        return NULL;

    if (g_chmod(tmux_path, 0755) != 0) {
        g_set_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                    errno, "Could not chmod tmux shim");
        return NULL;
    }

    return g_steal_pointer(&dir);
}

static char *
create_state_file(TmuxCompatState *state, GError **error)
{
    GKeyFile *kf;
    g_autofree char *tmp_path = NULL;
    g_autofree char *data = NULL;
    gsize len = 0;
    int fd;

    fd = g_file_open_tmp("prettymux-claude-teams-XXXXXX.ini", &tmp_path, error);
    if (fd < 0)
        return NULL;
    prettymux_close(fd);

    kf = g_key_file_new();
    g_key_file_set_integer(kf, "session", "workspace", state->workspace);
    if (state->leader_pane)
        g_key_file_set_string(kf, "session", "leader_pane", state->leader_pane);
    if (state->last_column_pane)
        g_key_file_set_string(kf, "session", "last_column_pane",
                              state->last_column_pane);
    if (state->layout)
        g_key_file_set_string(kf, "session", "layout", state->layout);

    data = g_key_file_to_data(kf, &len, NULL);
    g_key_file_unref(kf);

    if (!g_file_set_contents(tmp_path, data, (gssize)len, error))
        return NULL;

    return g_steal_pointer(&tmp_path);
}

static gboolean
load_state_from_env(TmuxCompatState *state, GError **error)
{
    const char *path = g_getenv("PRETTYMUX_TMUX_COMPAT_STATE");
    g_autofree char *data = NULL;
    gsize len = 0;
    GKeyFile *kf;

    tmux_state_clear(state);
    if (!path || !path[0]) {
        set_error_literal(error, "PRETTYMUX_TMUX_COMPAT_STATE is not set.");
        return FALSE;
    }

    if (!g_file_get_contents(path, &data, &len, error))
        return FALSE;

    kf = g_key_file_new();
    if (!g_key_file_load_from_data(kf, data, (gssize)len, G_KEY_FILE_NONE, error)) {
        g_key_file_unref(kf);
        return FALSE;
    }

    state->workspace = g_key_file_get_integer(kf, "session", "workspace", NULL);
    state->leader_pane = g_key_file_get_string(kf, "session", "leader_pane", NULL);
    state->last_column_pane = g_key_file_get_string(kf, "session",
                                                    "last_column_pane", NULL);
    state->layout = g_key_file_get_string(kf, "session", "layout", NULL);
    g_key_file_unref(kf);
    return TRUE;
}

static gboolean
save_state_to_env(const TmuxCompatState *state, GError **error)
{
    const char *path = g_getenv("PRETTYMUX_TMUX_COMPAT_STATE");
    GKeyFile *kf;
    g_autofree char *data = NULL;
    gsize len = 0;

    if (!path || !path[0]) {
        set_error_literal(error, "PRETTYMUX_TMUX_COMPAT_STATE is not set.");
        return FALSE;
    }

    kf = g_key_file_new();
    g_key_file_set_integer(kf, "session", "workspace", state->workspace);
    if (state->leader_pane && state->leader_pane[0])
        g_key_file_set_string(kf, "session", "leader_pane", state->leader_pane);
    if (state->last_column_pane && state->last_column_pane[0])
        g_key_file_set_string(kf, "session", "last_column_pane",
                              state->last_column_pane);
    if (state->layout && state->layout[0])
        g_key_file_set_string(kf, "session", "layout", state->layout);

    data = g_key_file_to_data(kf, &len, NULL);
    g_key_file_unref(kf);
    return g_file_set_contents(path, data, (gssize)len, error);
}

static void
tmux_state_clear(TmuxCompatState *state)
{
    if (!state)
        return;
    g_clear_pointer(&state->leader_pane, g_free);
    g_clear_pointer(&state->last_column_pane, g_free);
    g_clear_pointer(&state->layout, g_free);
    state->workspace = 0;
}

static gboolean
claude_has_teammate_mode(char **args, int argc)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "--teammate-mode") == 0 ||
            g_str_has_prefix(args[i], "--teammate-mode="))
            return TRUE;
    }
    return FALSE;
}

static char **
build_claude_argv(const char *claude_path, int argc, char **argv)
{
    gboolean has_mode = claude_has_teammate_mode(argv, argc);
    int extra = has_mode ? 0 : 2;
    char **result = g_new0(char *, argc + extra + 2);
    int out = 0;

    result[out++] = g_strdup(claude_path);
    if (!has_mode) {
        result[out++] = g_strdup("--teammate-mode");
        result[out++] = g_strdup("auto");
    }
    for (int i = 0; i < argc; i++)
        result[out++] = g_strdup(argv[i]);
    result[out] = NULL;
    return result;
}

static gboolean
parse_pane_target(const char *target, char **pane_id_out)
{
    const char *value = target;

    if ((!value || !value[0]) && g_getenv("TMUX_PANE"))
        value = g_getenv("TMUX_PANE");
    if (!value || !value[0])
        return FALSE;

    if (value[0] == '%')
        value++;

    if (!value[0])
        return FALSE;

    *pane_id_out = g_strdup(value);
    return TRUE;
}

static gboolean
ipc_type_text(int workspace, const char *pane_id, const char *text,
              GError **error)
{
    g_autofree char *escaped_pane = json_escape_string(pane_id);
    g_autofree char *escaped_text = json_escape_string(text);
    g_autofree char *msg = g_strdup_printf(
        "{\"command\":\"type\",\"workspace\":%d,\"paneId\":\"%s\",\"text\":\"%s\"}",
        workspace, escaped_pane, escaped_text);
    JsonParser *parser = ipc_request(msg, error);
    if (!parser)
        return FALSE;
    g_object_unref(parser);
    return TRUE;
}

static gboolean
ipc_type_text_with_retry(int workspace, const char *pane_id, const char *text,
                         GError **error)
{
    static const guint retry_sleep_us = 100 * 1000;
    static const guint retry_attempts = 20;

    for (guint attempt = 0; attempt < retry_attempts; attempt++) {
        GError *local_error = NULL;

        if (ipc_type_text(workspace, pane_id, text, &local_error))
            return TRUE;

        if (!local_error)
            continue;

        if (g_strcmp0(local_error->message, "no terminal found") != 0 &&
            g_strcmp0(local_error->message, "terminal has no surface") != 0) {
            g_propagate_error(error, local_error);
            return FALSE;
        }

        if (attempt + 1 == retry_attempts) {
            g_propagate_error(error, local_error);
            return FALSE;
        }

        g_clear_error(&local_error);
        g_usleep(retry_sleep_us);
    }

    set_error_literal(error, "type retry exhausted");
    return FALSE;
}

static char *
ipc_read_text(int workspace, const char *pane_id, gboolean scrollback,
              int lines, GError **error)
{
    g_autofree char *escaped_pane = json_escape_string(pane_id);
    g_autofree char *msg = g_strdup_printf(
        "{\"command\":\"pane.read_text\",\"workspace\":%d,\"paneId\":\"%s\",\"scrollback\":%s,\"lines\":%d}",
        workspace, escaped_pane, scrollback ? "true" : "false", lines);
    JsonParser *parser = ipc_request(msg, error);
    JsonObject *root;
    char *result;

    if (!parser)
        return NULL;

    root = json_node_get_object(json_parser_get_root(parser));
    result = g_strdup(
        json_object_get_string_member_with_default(root, "text", ""));
    g_object_unref(parser);
    return result;
}

static gboolean
ipc_new_tab(GError **error)
{
    JsonParser *parser = ipc_request("{\"command\":\"tab.new\"}", error);
    if (!parser)
        return FALSE;
    g_object_unref(parser);
    return TRUE;
}

static gboolean
parse_workspace_target(const char *target, int *workspace_out)
{
    const char *value = target;
    const char *env_value;

    if (value && value[0]) {
        const char *colon = strrchr(value, ':');
        if (colon && colon[1]) {
            *workspace_out = (int)g_ascii_strtoll(colon + 1, NULL, 10);
            return TRUE;
        }
        if (g_ascii_isdigit(value[0]) || value[0] == '-') {
            *workspace_out = (int)g_ascii_strtoll(value, NULL, 10);
            return TRUE;
        }
    }

    env_value = g_getenv("PRETTYMUX_TMUX_WORKSPACE");
    if (env_value && env_value[0]) {
        *workspace_out = (int)g_ascii_strtoll(env_value, NULL, 10);
        return TRUE;
    }

    return FALSE;
}

static char *
find_non_leader_pane_id(JsonObject *root, const char *leader_pane)
{
    JsonArray *panes = json_object_get_array_member(root, "panes");

    if (!panes)
        return NULL;

    for (guint i = 0; i < json_array_get_length(panes); i++) {
        JsonObject *pane = json_array_get_object_element(panes, i);
        const char *pane_id;

        if (!pane)
            continue;
        pane_id = json_object_get_string_member_with_default(pane, "id", "");
        if (pane_id[0] && g_strcmp0(pane_id, leader_pane) != 0)
            return g_strdup(pane_id);
    }

    return NULL;
}

static char *
replace_token(const char *input, const char *token, const char *value)
{
    const char *cursor;
    gsize token_len;
    GString *out;

    if (!input)
        return g_strdup("");
    if (!token || !token[0])
        return g_strdup(input);

    token_len = strlen(token);
    cursor = input;
    out = g_string_new("");

    while (*cursor) {
        const char *match = strstr(cursor, token);
        if (!match) {
            g_string_append(out, cursor);
            break;
        }

        g_string_append_len(out, cursor, (gssize)(match - cursor));
        g_string_append(out, value ? value : "");
        cursor = match + token_len;
    }

    return g_string_free(out, FALSE);
}

static char *
render_tmux_format(const char *format, int workspace, const char *pane_id)
{
    g_autofree char *pane_ref = g_strdup_printf("%%%s", pane_id ? pane_id : "");
    g_autofree char *workspace_str = g_strdup_printf("%d", workspace);
    g_autofree char *window_id = g_strdup_printf("@%d", workspace);
    g_autofree char *step1 = NULL;
    g_autofree char *step2 = NULL;
    g_autofree char *step3 = NULL;
    g_autofree char *step4 = NULL;
    g_autofree char *step5 = NULL;
    GRegex *unknown_format = NULL;
    char *result = NULL;

    if (!format || !format[0])
        return g_strdup("");

    step1 = replace_token(format, "#{session_name}", "prettymux");
    step2 = replace_token(step1, "#{session_id}", "$0");
    step3 = replace_token(step2, "#{window_index}", workspace_str);
    step4 = replace_token(step3, "#{window_id}", window_id);
    step5 = replace_token(step4, "#{pane_id}", pane_ref);
    result = replace_token(step5, "#{client_control_mode}", "0");

    unknown_format = g_regex_new("#\\{[^}]+\\}", 0, 0, NULL);
    if (unknown_format) {
        char *cleaned = g_regex_replace_literal(unknown_format, result, -1, 0,
                                                "", 0, NULL);
        g_regex_unref(unknown_format);
        g_free(result);
        result = cleaned;
    }

    return result;
}

static int
tmux_display_message(char **args, int argc)
{
    const char *target = NULL;
    const char *format = "";
    int workspace = 0;
    g_autofree char *pane_id = NULL;
    g_autofree char *rendered = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-t") == 0 && i + 1 < argc) {
            target = args[++i];
        } else if (strcmp(args[i], "-p") == 0) {
            /* -p is a boolean flag (print to stdout), not a value flag */
        } else if (strcmp(args[i], "-F") == 0 && i + 1 < argc) {
            format = args[++i];
        } else if (args[i][0] != '-') {
            format = args[i];
        }
    }

    if (!parse_workspace_target(target, &workspace)) {
        g_printerr("prettymux __tmux-compat: could not resolve workspace target\n");
        return 1;
    }

    if (!parse_pane_target(target, &pane_id))
        pane_id = g_strdup("");

    rendered = render_tmux_format(format, workspace, pane_id);
    g_print("%s\n", rendered);
    return 0;
}

static char *
tmux_build_shell_command_text(char **args, int argc, const char *cwd)
{
    GString *positional = g_string_new("");
    GString *result;

    /* Collect positional args (non-flag arguments after all flags) */
    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-h") == 0 || strcmp(args[i], "-v") == 0 ||
            strcmp(args[i], "-P") == 0 || strcmp(args[i], "-b") == 0 ||
            strcmp(args[i], "-d") == 0) {
            continue;
        }
        if ((strcmp(args[i], "-t") == 0 || strcmp(args[i], "-F") == 0 ||
             strcmp(args[i], "-l") == 0 || strcmp(args[i], "-c") == 0 ||
             strcmp(args[i], "-e") == 0) &&
            i + 1 < argc) {
            i++;
            continue;
        }
        if (args[i][0] == '-')
            continue;
        if (positional->len > 0)
            g_string_append_c(positional, ' ');
        g_string_append(positional, args[i]);
    }

    if (positional->len == 0 && (!cwd || !cwd[0])) {
        g_string_free(positional, TRUE);
        return NULL;
    }

    result = g_string_new("");
    if (cwd && cwd[0]) {
        g_string_append(result, "cd -- '");
        g_string_append(result, cwd);
        g_string_append_c(result, '\'');
        if (positional->len > 0)
            g_string_append(result, " && ");
    }
    if (positional->len > 0)
        g_string_append(result, positional->str);
    g_string_append_c(result, '\r');
    g_string_free(positional, TRUE);
    return g_string_free(result, FALSE);
}

static int
tmux_split_window(char **args, int argc)
{
    gboolean horizontal = FALSE;
    gboolean print_target = FALSE;
    const char *target = NULL;
    const char *format = "#{pane_id}";
    const char *cwd = NULL;
    const char *direction = "down";
    g_autofree char *target_pane_id = NULL;
    g_autofree char *new_pane_id = NULL;
    g_autofree char *rendered = NULL;
    g_autofree char *shell_text = NULL;
    JsonParser *parser = NULL;
    JsonObject *root;
    TmuxCompatState state = {0};
    GError *error = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-h") == 0) {
            horizontal = TRUE;
        } else if (strcmp(args[i], "-P") == 0) {
            print_target = TRUE;
        } else if (strcmp(args[i], "-t") == 0 && i + 1 < argc) {
            target = args[++i];
        } else if (strcmp(args[i], "-F") == 0 && i + 1 < argc) {
            format = args[++i];
        } else if (strcmp(args[i], "-l") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(args[i], "-c") == 0 && i + 1 < argc) {
            cwd = args[++i];
        }
    }

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    if (!parse_pane_target(target, &target_pane_id)) {
        g_printerr("prettymux __tmux-compat: split-window requires a pane target\n");
        tmux_state_clear(&state);
        return 1;
    }

    if (horizontal)
        direction = "right";

    if (horizontal &&
        g_strcmp0(state.layout, "main-vertical") == 0 &&
        state.leader_pane &&
        g_strcmp0(target_pane_id, state.leader_pane) == 0 &&
        state.last_column_pane &&
        state.last_column_pane[0]) {
        g_free(target_pane_id);
        target_pane_id = g_strdup(state.last_column_pane);
        direction = "down";
    }

    parser = ipc_pane_split(state.workspace, target_pane_id, direction, &error);
    if (!parser) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        tmux_state_clear(&state);
        return 1;
    }

    root = json_node_get_object(json_parser_get_root(parser));
    new_pane_id = g_strdup(
        json_object_get_string_member_with_default(root, "paneId", ""));
    if (!new_pane_id[0]) {
        g_printerr("prettymux __tmux-compat: split did not return paneId\n");
        g_object_unref(parser);
        tmux_state_clear(&state);
        return 1;
    }

    g_free(state.last_column_pane);
    state.last_column_pane = g_strdup(new_pane_id);
    if (!save_state_to_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
    }

    /* Send shell command text to the new pane (like tmux's shell-command arg) */
    shell_text = tmux_build_shell_command_text(args, argc, cwd);
    if (shell_text && shell_text[0]) {
        ipc_type_text_with_retry(state.workspace, new_pane_id, shell_text,
                                 NULL);
    }

    if (print_target) {
        rendered = render_tmux_format(format, state.workspace, new_pane_id);
        g_print("%s\n", rendered);
    }

    ipc_equalize(state.workspace,
                 g_strcmp0(state.layout, "main-vertical") == 0
                     ? "vertical"
                     : NULL,
                 NULL);

    g_object_unref(parser);
    tmux_state_clear(&state);
    return 0;
}

static int
tmux_select_layout(char **args, int argc)
{
    const char *target = NULL;
    const char *layout = NULL;
    TmuxCompatState state = {0};
    GError *error = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-t") == 0 && i + 1 < argc)
            target = args[++i];
        else if (!layout)
            layout = args[i];
    }

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    if (target && target[0])
        parse_workspace_target(target, &state.workspace);

    g_free(state.layout);
    state.layout = g_strdup(layout ? layout : "");

    if (!save_state_to_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        tmux_state_clear(&state);
        return 1;
    }

    if (g_strcmp0(state.layout, "main-vertical") == 0)
        ipc_equalize(state.workspace, "vertical", NULL);
    else
        ipc_equalize(state.workspace, NULL, NULL);

    tmux_state_clear(&state);
    return 0;
}

static int
tmux_resize_pane(char **args, int argc)
{
    const char *target = NULL;
    const char *x_value = NULL;
    g_autofree char *pane_id = NULL;
    TmuxCompatState state = {0};
    GError *error = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-t") == 0 && i + 1 < argc)
            target = args[++i];
        else if (strcmp(args[i], "-x") == 0 && i + 1 < argc)
            x_value = args[++i];
    }

    if (!x_value)
        return 0;

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    if (!parse_pane_target(target, &pane_id)) {
        tmux_state_clear(&state);
        return 0;
    }

    if (strchr(x_value, '%')) {
        double percent = g_ascii_strtod(x_value, NULL);
        if (!ipc_resize_percent(state.workspace, pane_id, 'x', percent, &error)) {
            g_clear_error(&error);
        }
    }

    tmux_state_clear(&state);
    return 0;
}

static int
tmux_select_pane(char **args, int argc)
{
    const char *target = NULL;
    g_autofree char *pane_id = NULL;
    TmuxCompatState state = {0};
    GError *error = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-t") == 0 && i + 1 < argc)
            target = args[++i];
    }

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    if (!parse_pane_target(target, &pane_id)) {
        tmux_state_clear(&state);
        return 1;
    }

    if (!ipc_pane_focus(state.workspace, pane_id, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        tmux_state_clear(&state);
        return 1;
    }

    tmux_state_clear(&state);
    return 0;
}

static int
tmux_list_panes(char **args, int argc)
{
    const char *target = NULL;
    const char *format = "#{pane_id}";
    int workspace = 0;
    JsonParser *parser = NULL;
    JsonObject *root;
    JsonArray *panes;
    GError *error = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-t") == 0 && i + 1 < argc)
            target = args[++i];
        else if (strcmp(args[i], "-F") == 0 && i + 1 < argc)
            format = args[++i];
    }

    if (!parse_workspace_target(target, &workspace)) {
        g_printerr("prettymux __tmux-compat: could not resolve workspace target\n");
        return 1;
    }

    parser = ipc_pane_list(workspace, &error);
    if (!parser) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    root = json_node_get_object(json_parser_get_root(parser));
    panes = json_object_get_array_member(root, "panes");
    if (panes) {
        for (guint i = 0; i < json_array_get_length(panes); i++) {
            JsonObject *pane = json_array_get_object_element(panes, i);
            const char *pane_id =
                json_object_get_string_member_with_default(pane, "id", "");
            g_autofree char *rendered =
                render_tmux_format(format, workspace, pane_id);
            g_print("%s\n", rendered);
        }
    }

    g_object_unref(parser);
    return 0;
}

static int
tmux_kill_pane(char **args, int argc)
{
    const char *target = NULL;
    g_autofree char *pane_id = NULL;
    g_autofree char *replacement = NULL;
    JsonParser *parser = NULL;
    JsonObject *root;
    TmuxCompatState state = {0};
    GError *error = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-t") == 0 && i + 1 < argc)
            target = args[++i];
    }

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    if (!parse_pane_target(target, &pane_id)) {
        tmux_state_clear(&state);
        return 1;
    }

    if (!ipc_pane_close(state.workspace, pane_id, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        tmux_state_clear(&state);
        return 1;
    }

    parser = ipc_pane_list(state.workspace, &error);
    if (parser) {
        root = json_node_get_object(json_parser_get_root(parser));
        replacement = find_non_leader_pane_id(root, state.leader_pane);
        g_object_unref(parser);
    } else {
        g_clear_error(&error);
    }

    if (g_strcmp0(state.last_column_pane, pane_id) == 0) {
        g_free(state.last_column_pane);
        state.last_column_pane = g_steal_pointer(&replacement);
    }

    ipc_equalize(state.workspace,
                 g_strcmp0(state.layout, "main-vertical") == 0
                     ? "vertical"
                     : NULL,
                 NULL);

    if (!save_state_to_env(&state, &error)) {
        g_clear_error(&error);
    }

    tmux_state_clear(&state);
    return 0;
}

static const char *
tmux_special_key_text(const char *token)
{
    if (!token || !token[0])
        return NULL;

    if (g_ascii_strcasecmp(token, "enter") == 0 ||
        g_ascii_strcasecmp(token, "c-m") == 0 ||
        g_ascii_strcasecmp(token, "kpenter") == 0)
        return "\r";
    if (g_ascii_strcasecmp(token, "tab") == 0 ||
        g_ascii_strcasecmp(token, "c-i") == 0)
        return "\t";
    if (g_ascii_strcasecmp(token, "space") == 0)
        return " ";
    if (g_ascii_strcasecmp(token, "bspace") == 0 ||
        g_ascii_strcasecmp(token, "backspace") == 0)
        return "\177";
    if (g_ascii_strcasecmp(token, "escape") == 0 ||
        g_ascii_strcasecmp(token, "esc") == 0 ||
        g_ascii_strcasecmp(token, "c-[") == 0)
        return "\033";
    if (g_ascii_strcasecmp(token, "c-c") == 0)
        return "\003";
    if (g_ascii_strcasecmp(token, "c-d") == 0)
        return "\004";
    if (g_ascii_strcasecmp(token, "c-z") == 0)
        return "\032";
    if (g_ascii_strcasecmp(token, "c-l") == 0)
        return "\014";

    return NULL;
}

static char *
tmux_send_keys_text(char **tokens, int count, gboolean literal)
{
    GString *result = g_string_new("");
    gboolean pending_space = FALSE;

    if (literal) {
        for (int i = 0; i < count; i++) {
            if (i > 0)
                g_string_append_c(result, ' ');
            g_string_append(result, tokens[i]);
        }
        return g_string_free(result, FALSE);
    }

    for (int i = 0; i < count; i++) {
        const char *special = tmux_special_key_text(tokens[i]);
        if (special) {
            g_string_append(result, special);
            pending_space = FALSE;
            continue;
        }
        if (pending_space)
            g_string_append_c(result, ' ');
        g_string_append(result, tokens[i]);
        pending_space = TRUE;
    }

    return g_string_free(result, FALSE);
}

static int
tmux_send_keys(char **args, int argc)
{
    const char *target = NULL;
    gboolean literal = FALSE;
    g_autofree char *pane_id = NULL;
    g_autofree char *text = NULL;
    TmuxCompatState state = {0};
    GPtrArray *tokens;
    GError *error = NULL;
    int exit_status = 0;

    tokens = g_ptr_array_new();
    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-t") == 0 && i + 1 < argc) {
            target = args[++i];
        } else if (strcmp(args[i], "-l") == 0) {
            literal = TRUE;
        } else {
            g_ptr_array_add(tokens, args[i]);
        }
    }
    g_ptr_array_add(tokens, NULL);

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        g_ptr_array_free(tokens, TRUE);
        return 1;
    }

    if (!parse_pane_target(target, &pane_id)) {
        tmux_state_clear(&state);
        g_ptr_array_free(tokens, TRUE);
        return 1;
    }

    text = tmux_send_keys_text((char **)tokens->pdata,
                               (int)tokens->len - 1, literal);
    if (text && text[0] &&
        !ipc_type_text_with_retry(state.workspace, pane_id, text, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        exit_status = 1;
    }

    tmux_state_clear(&state);
    g_ptr_array_free(tokens, TRUE);
    return exit_status;
}

static int
tmux_capture_pane(char **args, int argc)
{
    const char *target = NULL;
    gboolean print_text = FALSE;
    gboolean scrollback = TRUE;
    int lines = 0;
    g_autofree char *pane_id = NULL;
    g_autofree char *text = NULL;
    TmuxCompatState state = {0};
    GError *error = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-t") == 0 && i + 1 < argc) {
            target = args[++i];
        } else if (strcmp(args[i], "-p") == 0) {
            print_text = TRUE;
        } else if (strcmp(args[i], "-S") == 0 && i + 1 < argc) {
            int start = (int)g_ascii_strtoll(args[++i], NULL, 10);
            if (start < 0)
                lines = -start;
        } else if (strcmp(args[i], "-E") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(args[i], "-J") == 0 ||
                   strcmp(args[i], "-N") == 0) {
        }
    }

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    if (!parse_pane_target(target, &pane_id)) {
        tmux_state_clear(&state);
        return 1;
    }

    text = ipc_read_text(state.workspace, pane_id, scrollback, lines, &error);
    if (!text) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        tmux_state_clear(&state);
        return 1;
    }

    if (print_text)
        g_print("%s", text);

    tmux_state_clear(&state);
    return 0;
}

static int
tmux_new_window(char **args, int argc)
{
    const char *format = "#{pane_id}";
    TmuxCompatState state = {0};
    g_autofree char *rendered = NULL;
    GError *error = NULL;
    int exit_status = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-F") == 0 && i + 1 < argc)
            format = args[++i];
    }

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    if (!ipc_new_tab(&error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        tmux_state_clear(&state);
        return 1;
    }

    if (state.leader_pane && state.leader_pane[0]) {
        rendered = render_tmux_format(format, state.workspace, state.leader_pane);
        g_print("%s\n", rendered);
    }

    tmux_state_clear(&state);
    return exit_status;
}

static int
tmux_list_windows(char **args, int argc)
{
    const char *format = "#{window_index}: #{window_name}";
    TmuxCompatState state = {0};
    GError *error = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(args[i], "-F") == 0 && i + 1 < argc)
            format = args[++i];
    }

    if (!load_state_from_env(&state, &error)) {
        g_printerr("prettymux __tmux-compat: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }

    /* Emit a single "window" entry representing the current workspace */
    {
        g_autofree char *rendered =
            render_tmux_format(format, state.workspace, state.leader_pane);
        g_print("%s\n", rendered);
    }

    tmux_state_clear(&state);
    return 0;
}

static gboolean
run_tmux_compat(int argc, char **argv, int *exit_code)
{
    const char *command;

    if (exit_code)
        *exit_code = 1;

    if (argc <= 0 || !argv[0]) {
        g_printerr("Usage: prettymux __tmux-compat <tmux-command> [args...]\n");
        return TRUE;
    }

    command = argv[0];
    if (strcmp(command, "-V") == 0 || strcmp(command, "-v") == 0) {
        g_print("tmux 3.4\n");
        if (exit_code)
            *exit_code = 0;
        return TRUE;
    }

    if (strcmp(command, "display-message") == 0 ||
        strcmp(command, "display") == 0) {
        if (exit_code)
            *exit_code = tmux_display_message(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "split-window") == 0 ||
        strcmp(command, "splitw") == 0) {
        if (exit_code)
            *exit_code = tmux_split_window(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "select-layout") == 0) {
        if (exit_code)
            *exit_code = tmux_select_layout(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "resize-pane") == 0 ||
        strcmp(command, "resizep") == 0) {
        if (exit_code)
            *exit_code = tmux_resize_pane(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "select-pane") == 0 ||
        strcmp(command, "selectp") == 0) {
        if (exit_code)
            *exit_code = tmux_select_pane(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "list-panes") == 0 ||
        strcmp(command, "lsp") == 0) {
        if (exit_code)
            *exit_code = tmux_list_panes(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "list-windows") == 0 ||
        strcmp(command, "lsw") == 0) {
        if (exit_code)
            *exit_code = tmux_list_windows(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "kill-pane") == 0 ||
        strcmp(command, "killp") == 0) {
        if (exit_code)
            *exit_code = tmux_kill_pane(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "send-keys") == 0 ||
        strcmp(command, "send") == 0) {
        if (exit_code)
            *exit_code = tmux_send_keys(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "capture-pane") == 0 ||
        strcmp(command, "capturep") == 0) {
        if (exit_code)
            *exit_code = tmux_capture_pane(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "new-window") == 0 ||
        strcmp(command, "neww") == 0) {
        if (exit_code)
            *exit_code = tmux_new_window(argv + 1, argc - 1);
        return TRUE;
    }

    if (strcmp(command, "has-session") == 0 ||
        strcmp(command, "has") == 0) {
        if (exit_code)
            *exit_code = 0;
        return TRUE;
    }

    /* No-ops: commands that should succeed silently */
    if (strcmp(command, "set-option") == 0 ||
        strcmp(command, "set") == 0 ||
        strcmp(command, "set-window-option") == 0 ||
        strcmp(command, "setw") == 0 ||
        strcmp(command, "refresh-client") == 0 ||
        strcmp(command, "source-file") == 0 ||
        strcmp(command, "attach-session") == 0 ||
        strcmp(command, "detach-client") == 0 ||
        strcmp(command, "last-pane") == 0 ||
        strcmp(command, "last-window") == 0 ||
        strcmp(command, "next-window") == 0 ||
        strcmp(command, "previous-window") == 0 ||
        strcmp(command, "set-hook") == 0 ||
        strcmp(command, "set-buffer") == 0 ||
        strcmp(command, "list-buffers") == 0 ||
        strcmp(command, "rename-window") == 0 ||
        strcmp(command, "renamew") == 0 ||
        strcmp(command, "kill-window") == 0 ||
        strcmp(command, "killw") == 0 ||
        strcmp(command, "select-window") == 0 ||
        strcmp(command, "selectw") == 0 ||
        strcmp(command, "wait-for") == 0 ||
        strcmp(command, "show-buffer") == 0 ||
        strcmp(command, "showb") == 0 ||
        strcmp(command, "save-buffer") == 0 ||
        strcmp(command, "saveb") == 0) {
        if (exit_code)
            *exit_code = 0;
        return TRUE;
    }

    g_printerr("prettymux __tmux-compat: unsupported tmux command: %s\n",
               command);
    /* Return 0 even for unknown commands to avoid breaking agents */
    if (exit_code)
        *exit_code = 0;
    return TRUE;
}

static gboolean
run_claude_teams(int argc, char **argv, int *exit_code)
{
    g_autofree char *claude_path = NULL;
    g_autofree char *exe_path = NULL;
    g_autofree char *shim_dir = NULL;
    g_autofree char *socket_path = NULL;
    g_autofree char *state_path = NULL;
    g_autofree char *restore_module_path = NULL;
    g_autofree char *merged_node_options = NULL;
    g_autofree char *workspace_str = NULL;
    g_autofree char *fake_tmux = NULL;
    g_autofree char *original_path_copy = g_strdup(g_getenv("PATH"));
    g_autofree char *original_node_options =
        g_strdup(g_getenv("NODE_OPTIONS"));
    g_auto(GStrv) launch_argv = NULL;
    JsonParser *parser = NULL;
    JsonObject *root;
    TmuxCompatState state = {0};
    GError *error = NULL;
    const char *original_path = original_path_copy;

    if (exit_code)
        *exit_code = 1;

#ifdef G_OS_WIN32
    g_printerr("prettymux claude-teams: not supported on Windows\n");
    return TRUE;
#endif

    socket_path = find_socket_path(&error);
    if (!socket_path) {
        g_printerr("prettymux claude-teams: %s\n", error->message);
        g_clear_error(&error);
        return TRUE;
    }
    g_setenv("PRETTYMUX_SOCKET", socket_path, TRUE);

    parser = ipc_workspace_current(&error);
    if (!parser) {
        g_printerr("prettymux claude-teams: %s\n", error->message);
        g_clear_error(&error);
        return TRUE;
    }

    root = json_node_get_object(json_parser_get_root(parser));
    state.workspace = (int)json_object_get_int_member_with_default(root, "index", -1);
    state.leader_pane = g_strdup(
        json_object_get_string_member_with_default(root, "paneId", ""));
    if (state.workspace < 0 || !state.leader_pane[0]) {
        g_printerr("prettymux claude-teams: could not determine current pane\n");
        g_object_unref(parser);
        tmux_state_clear(&state);
        return TRUE;
    }
    g_object_unref(parser);

    {
        char *resolved_pane =
            ipc_resolve_leader_pane(state.workspace, state.leader_pane, &error);
        if (!resolved_pane || !resolved_pane[0]) {
            g_printerr("prettymux claude-teams: %s\n",
                       error ? error->message : "no live panes found");
            g_clear_error(&error);
            g_free(resolved_pane);
            tmux_state_clear(&state);
            return TRUE;
        }
        g_free(state.leader_pane);
        state.leader_pane = g_strdup(resolved_pane);
        g_free(resolved_pane);
    }

    exe_path = current_executable_path();
    shim_dir = ensure_tmux_shim_dir(exe_path, &error);
    if (!shim_dir) {
        g_printerr("prettymux claude-teams: %s\n", error->message);
        g_clear_error(&error);
        tmux_state_clear(&state);
        return TRUE;
    }

    claude_path = find_executable_in_path("claude", original_path, shim_dir);
    if (!claude_path) {
        g_printerr("prettymux claude-teams: claude not found in PATH\n");
        tmux_state_clear(&state);
        return TRUE;
    }

    state_path = create_state_file(&state, &error);
    if (!state_path) {
        g_printerr("prettymux claude-teams: %s\n", error->message);
        g_clear_error(&error);
        tmux_state_clear(&state);
        return TRUE;
    }

    launch_argv = build_claude_argv(claude_path, argc, argv);

    g_setenv("CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS", "1", TRUE);
    g_unsetenv("CLAUDECODE");
    g_setenv("PRETTYMUX_CLAUDE_TEAMS_BIN", exe_path ? exe_path : "prettymux",
             TRUE);
    g_setenv("PRETTYMUX_TMUX_COMPAT_STATE", state_path, TRUE);
    workspace_str = g_strdup_printf("%d", state.workspace);
    g_setenv("PRETTYMUX_TMUX_WORKSPACE", workspace_str, TRUE);

    /* Set cmux-compatible env vars so Claude Code recognizes the
       teammate-capable environment (it may check these). */
    g_setenv("CMUX_SOCKET_PATH", socket_path, TRUE);
    g_setenv("CMUX_SOCKET", socket_path, TRUE);
    g_setenv("CMUX_WORKSPACE_ID", workspace_str, TRUE);
    g_setenv("CMUX_SURFACE_ID", state.leader_pane, TRUE);

    fake_tmux = g_strdup_printf("/tmp/prettymux-claude-teams/%d,%d,%s",
                                state.workspace, state.workspace,
                                state.leader_pane);
    g_setenv("TMUX", fake_tmux, TRUE);
    {
        g_autofree char *tmux_pane = g_strdup_printf("%%%s", state.leader_pane);
        g_setenv("TMUX_PANE", tmux_pane, TRUE);
    }
    if (!g_getenv("PRETTYMUX_CLAUDE_TEAMS_TERM"))
        g_setenv("TERM", "screen-256color", TRUE);
    else
        g_setenv("TERM", g_getenv("PRETTYMUX_CLAUDE_TEAMS_TERM"), TRUE);
    g_unsetenv("TERM_PROGRAM");
    if (!g_getenv("COLORTERM") || !g_getenv("COLORTERM")[0])
        g_setenv("COLORTERM", "truecolor", TRUE);

    restore_module_path = create_node_options_restore_module(&error);
    if (restore_module_path) {
        if (original_node_options)
            g_setenv("CMUX_ORIGINAL_NODE_OPTIONS_PRESENT", "1", TRUE);
        else
            g_setenv("CMUX_ORIGINAL_NODE_OPTIONS_PRESENT", "0", TRUE);

        if (original_node_options)
            g_setenv("CMUX_ORIGINAL_NODE_OPTIONS", original_node_options, TRUE);
        else
            g_unsetenv("CMUX_ORIGINAL_NODE_OPTIONS");

        merged_node_options =
            merge_node_options(original_node_options, restore_module_path);
        g_setenv("NODE_OPTIONS", merged_node_options, TRUE);
    } else {
        g_clear_error(&error);
    }

    /* Enable tmux shim logging if PRETTYMUX_TMUX_LOG is set, or auto-enable
       to a temp file for diagnostics. */
    if (!g_getenv("PRETTYMUX_TMUX_LOG")) {
        g_autofree char *log_path = g_build_filename(
            g_get_tmp_dir(), "prettymux-tmux-shim.log", NULL);
        g_setenv("PRETTYMUX_TMUX_LOG", log_path, TRUE);
    }

    if (original_path && original_path[0]) {
        g_autofree char *new_path =
            g_strdup_printf("%s:%s", shim_dir, original_path);
        g_setenv("PATH", new_path, TRUE);
    } else {
        g_setenv("PATH", shim_dir, TRUE);
    }

    g_printerr("prettymux claude-teams: launching claude\n"
               "  claude = %s\n"
               "  leader = %s (ws %d)\n"
               "  shim   = %s/tmux\n"
               "  log    = %s\n",
               claude_path, state.leader_pane, state.workspace,
               shim_dir, g_getenv("PRETTYMUX_TMUX_LOG"));

    prettymux_execv(claude_path, launch_argv);
    g_printerr("prettymux claude-teams: exec failed: %s\n", g_strerror(errno));
    tmux_state_clear(&state);
    if (exit_code)
        *exit_code = 1;
    return TRUE;
}
