#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "app_state.h"

#include <glib.h>

#define PRETTYMUX_DEFAULT_INSTANCE_ID "default"

static gboolean
instance_id_char_allowed(char c)
{
    return g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.';
}

static gboolean
instance_id_is_valid(const char *instance_id)
{
    if (!instance_id || !instance_id[0])
        return FALSE;

    for (const char *p = instance_id; *p; p++) {
        if (!instance_id_char_allowed(*p))
            return FALSE;
    }

    return TRUE;
}

static void
sanitize_instance_id(const char *input, char *out, gsize out_len)
{
    gsize out_idx = 0;

    if (!out || out_len == 0)
        return;

    out[0] = '\0';
    if (!input || !input[0])
        return;

    for (const char *p = input; *p && out_idx + 1 < out_len; p++) {
        if (instance_id_char_allowed(*p))
            out[out_idx++] = *p;
    }
    out[out_idx] = '\0';
}

static void
assign_instance_id(AppState *state, const char *preferred_id)
{
    char base_id[sizeof(state->instance_id)] = {0};

    sanitize_instance_id(preferred_id, base_id, sizeof(base_id));
    if (!base_id[0])
        g_strlcpy(base_id, PRETTYMUX_DEFAULT_INSTANCE_ID, sizeof(base_id));
    g_strlcpy(state->instance_id, base_id, sizeof(state->instance_id));
}

static void
assign_default_instance_id(AppState *state)
{
    assign_instance_id(state, PRETTYMUX_DEFAULT_INSTANCE_ID);
}

static gboolean
derive_nested_instance_lane_id(char *out, gsize out_len)
{
    const char *env_lane_id = g_getenv("PRETTYMUX_CHILD_INSTANCE_ID");
    const char *terminal_id = g_getenv("PRETTYMUX_TERMINAL_ID");

    if (!out || out_len == 0)
        return FALSE;

    out[0] = '\0';

    /*
     * Explicit child ids let callers allocate stable same-lane child slots
     * without relying on live socket occupancy.
     */
    sanitize_instance_id(env_lane_id, out, out_len);
    if (out[0])
        return TRUE;

    sanitize_instance_id(terminal_id, out, out_len);
    if (out[0])
        return FALSE;

#ifndef G_OS_WIN32
    {
        const char *tty_path = ttyname(STDIN_FILENO);
        sanitize_instance_id(tty_path, out, out_len);
        if (out[0])
            return FALSE;
    }
#endif

    g_strlcpy(out, "shell", out_len);
    return FALSE;
}

static void
build_nested_child_instance_id(char *out,
                               gsize out_len,
                               const char *base_instance_id,
                               guint slot_index)
{
    if (!out || out_len == 0)
        return;

    if (!base_instance_id || !base_instance_id[0]) {
        out[0] = '\0';
        return;
    }

    if (slot_index <= 1) {
        g_strlcpy(out, base_instance_id, out_len);
        return;
    }

    g_snprintf(out, out_len, "%s-%u", base_instance_id, slot_index);
}

AppState *
app_state(void)
{
    static AppState state = {
        .ghostty_default_font_size = 0.0f,
        .main_window_active = FALSE,
        .terminal_search_total = -1,
        .terminal_search_selected = -1,
    };

    return &state;
}

const char *
app_state_get_instance_id(void)
{
    AppState *state = app_state();

    if (!state->instance_id[0])
        assign_default_instance_id(state);
    return state->instance_id;
}

void
app_state_set_instance_id(const char *instance_id)
{
    AppState *state = app_state();
    assign_instance_id(state, instance_id);
}

#ifndef G_OS_WIN32
/* Per-user socket directory: $XDG_RUNTIME_DIR when set, else /tmp. Must match
 * the resolution in socket_server.c / prettymux-open / prettymux_agent_cli. */
static const char *
socket_runtime_dir(void)
{
    const char *dir = g_getenv("XDG_RUNTIME_DIR");
    return (dir && dir[0]) ? dir : "/tmp";
}

static gboolean
socket_path_is_connectable(const char *path)
{
    int fd;
    struct sockaddr_un addr = {0};
    gboolean ok = FALSE;

    if (!path || !path[0])
        return FALSE;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return FALSE;

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

static gboolean
instance_id_has_live_socket(const char *instance_id)
{
    char socket_path[256];

    if (!instance_id || !instance_id[0])
        return FALSE;

    g_snprintf(socket_path, sizeof(socket_path), "%s/prettymux-%s.sock",
               socket_runtime_dir(), instance_id);
    return socket_path_is_connectable(socket_path);
}

static gboolean
instance_id_has_saved_session(const char *instance_id)
{
    g_autofree char *sessions_dir = NULL;
    g_autofree char *file_name = NULL;
    g_autofree char *session_path = NULL;

    if (!instance_id || !instance_id[0])
        return FALSE;

    sessions_dir =
        g_build_filename(g_get_home_dir(), ".prettymux", "sessions", NULL);
    if (g_strcmp0(instance_id, PRETTYMUX_DEFAULT_INSTANCE_ID) == 0)
        file_name = g_strdup("last-default.json");
    else
        file_name = g_strdup_printf("last-%s.json", instance_id);

    session_path = g_build_filename(sessions_dir, file_name, NULL);
    return g_file_test(session_path, G_FILE_TEST_EXISTS);
}

static gboolean
parse_instance_id_from_socket_name(const char *name, char *out, gsize out_len)
{
    const char *prefix = "prettymux-";
    const char *suffix = ".sock";
    gsize len;
    gsize prefix_len;
    gsize suffix_len;
    gsize id_len;

    if (!name || !out || out_len == 0)
        return FALSE;

    out[0] = '\0';
    len = strlen(name);
    prefix_len = strlen(prefix);
    suffix_len = strlen(suffix);
    if (len <= prefix_len + suffix_len)
        return FALSE;
    if (!g_str_has_prefix(name, prefix) || !g_str_has_suffix(name, suffix))
        return FALSE;

    id_len = len - prefix_len - suffix_len;
    if (id_len + 1 > out_len)
        return FALSE;

    memcpy(out, name + prefix_len, id_len);
    out[id_len] = '\0';
    return instance_id_is_valid(out);
}

static gboolean
parse_numeric_instance_id(const char *instance_id, unsigned long long *out_value)
{
    char *end = NULL;
    unsigned long long value;

    if (!instance_id || !instance_id[0])
        return FALSE;

    errno = 0;
    value = g_ascii_strtoull(instance_id, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return FALSE;

    if (out_value)
        *out_value = value;
    return TRUE;
}

static gint
compare_instance_id(const char *a, const char *b)
{
    unsigned long long a_num = 0;
    unsigned long long b_num = 0;
    gboolean a_is_numeric = parse_numeric_instance_id(a, &a_num);
    gboolean b_is_numeric = parse_numeric_instance_id(b, &b_num);

    if (a_is_numeric && b_is_numeric) {
        if (a_num < b_num)
            return -1;
        if (a_num > b_num)
            return 1;
    } else if (a_is_numeric != b_is_numeric) {
        return a_is_numeric ? -1 : 1;
    }

    return g_strcmp0(a, b);
}

static gint
compare_instance_id_ptr(gconstpointer a, gconstpointer b)
{
    const char *ia = *(const char *const *)a;
    const char *ib = *(const char *const *)b;
    return compare_instance_id(ia, ib);
}
#endif

static void
assign_nested_child_instance_id(AppState *state, const char *parent_instance_id)
{
    char parent_id[sizeof(state->instance_id)] = {0};
    char lane_id[sizeof(state->instance_id)] = {0};
    char base_candidate[sizeof(state->instance_id)] = {0};
    gboolean explicit_child_slot = FALSE;

    sanitize_instance_id(parent_instance_id, parent_id, sizeof(parent_id));
    if (!parent_id[0])
        g_strlcpy(parent_id, PRETTYMUX_DEFAULT_INSTANCE_ID,
                  sizeof(parent_id));

    explicit_child_slot = derive_nested_instance_lane_id(lane_id,
                                                         sizeof(lane_id));
    g_snprintf(base_candidate, sizeof(base_candidate), "%s-child-%s",
               parent_id, lane_id);

#ifdef G_OS_WIN32
    assign_instance_id(state, base_candidate);
#else
    if (explicit_child_slot) {
        assign_instance_id(state, base_candidate);
        return;
    }

    /*
     * Default nested launch policy:
     * - preserve an existing saved lane id when available and currently offline
     * - otherwise allocate the first currently unused child slot
     * This keeps same-lane child ids collision-free without random suffixes and
     * allows restart of previously saved child lanes while a sibling is live.
     */
    {
        gboolean base_live = instance_id_has_live_socket(base_candidate);
        gboolean base_saved = instance_id_has_saved_session(base_candidate);
        char first_free[sizeof(state->instance_id)] = {0};

        if (!base_live && base_saved) {
            assign_instance_id(state, base_candidate);
            return;
        }

        for (guint slot = 2; slot < 1000; slot++) {
            char candidate[sizeof(state->instance_id)] = {0};

            build_nested_child_instance_id(candidate, sizeof(candidate),
                                           base_candidate, slot);
            if (!candidate[0])
                continue;
            if (instance_id_has_live_socket(candidate))
                continue;
            if (instance_id_has_saved_session(candidate)) {
                assign_instance_id(state, candidate);
                return;
            }
            if (!first_free[0])
                g_strlcpy(first_free, candidate, sizeof(first_free));
        }

        if (!base_live && !base_saved) {
            assign_instance_id(state, base_candidate);
            return;
        }
        if (first_free[0]) {
            assign_instance_id(state, first_free);
            return;
        }
    }

    assign_instance_id(state, base_candidate);
#endif
}

void
app_state_init_instance_id_from_env(void)
{
    const char *env_instance_id = g_getenv("PRETTYMUX_INSTANCE_ID");
    const char *inside_prettymux = g_getenv("PRETTYMUX");
    const char *parent_socket = g_getenv("PRETTYMUX_SOCKET");

    if (!env_instance_id || !env_instance_id[0])
        return;

    if (inside_prettymux && inside_prettymux[0] &&
        parent_socket && parent_socket[0]) {
        assign_nested_child_instance_id(app_state(), env_instance_id);
        return;
    }

    app_state_set_instance_id(env_instance_id);
}

GPtrArray *
app_state_list_instances(void)
{
    GPtrArray *instances = g_ptr_array_new_with_free_func(g_free);

#ifdef G_OS_WIN32
    g_ptr_array_add(instances, g_strdup(app_state_get_instance_id()));
    return instances;
#else
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
    const char *runtime_dir = socket_runtime_dir();
    DIR *dir = opendir(runtime_dir);
    struct dirent *ent;

    if (!dir)
        return instances;

    while ((ent = readdir(dir)) != NULL) {
        char instance_id[128];
        char socket_path[256];
        int path_len;

        if (!parse_instance_id_from_socket_name(ent->d_name, instance_id,
                                                sizeof(instance_id)))
            continue;
        if (g_hash_table_contains(seen, instance_id))
            continue;

        path_len = g_snprintf(socket_path, sizeof(socket_path), "%s/%s",
                              runtime_dir, ent->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(socket_path))
            continue; /* path too long for the buffer */
        if (!socket_path_is_connectable(socket_path))
            continue;

        g_hash_table_add(seen, g_strdup(instance_id));
        g_ptr_array_add(instances, g_strdup(instance_id));
    }

    closedir(dir);

    if (instances->len > 1)
        g_ptr_array_sort(instances, compare_instance_id_ptr);

    return instances;
#endif
}
