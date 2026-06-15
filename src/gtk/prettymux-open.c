/*
 * prettymux-open.c - CLI tool for controlling a running prettymux instance
 *
 * Communicates with prettymux via a Unix domain socket (path from
 * PRETTYMUX_SOCKET env var).
 *
 * Usage:
 *   prettymux-open <url>                          Hand URL to system default handler
 *   prettymux-open --action <name> [--non-interactive]
 *                                                   Run an action (e.g. split.horizontal)
 *   prettymux-open --exec <cmd>                   Execute command in focused terminal
 *   prettymux-open --type <text>                  Type text into focused terminal
 *   prettymux-open --exec <cmd> -w 0 -p 1 -t 0   Target specific workspace/pane/tab
 *   prettymux-open --new-workspace [name]         Create a new workspace
 *   prettymux-open --new-tab                      Create a new terminal tab
 *   prettymux-open --move-tab ...                 Move a terminal tab to another pane
 *   prettymux-open --move-workspace ...           Move a workspace to another instance
 *   prettymux-open --select-tab -w 0 -p 1 -t 0    Select a specific terminal tab
 *   prettymux-open --set-layout <mode> [-w N]     Set layout (all workspaces or one target)
 *   prettymux-open --get-layout [-w N]            Get workspace layout mode
 *   prettymux-open --get-strip-state [-w N]       Get strip column state
 *   prettymux-open --set-workspace-status ...      Set structured status entry
 *   prettymux-open --clear-workspace-status ...    Clear status entry by id
 *   prettymux-open --list-workspace-status [-w N]  List structured status entries
 *   prettymux-open --list-workspaces              List all workspaces
 *   prettymux-open --list-tabs                    List workspaces/panes/tabs
 *   prettymux-open --list-actions                 List all available actions
 *   prettymux-open --quit                         Close prettymux cleanly
 *   prettymux-open --switch-workspace <n>         Switch to workspace N
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>

static const char *g_cli_target_instance = NULL;
static const char *g_cli_target_socket = NULL;
static char *json_escape(const char *s);

typedef enum {
    SOCKET_RESOLUTION_NONE = 0,
    SOCKET_RESOLUTION_NO_RUNNING_INSTANCE,
    SOCKET_RESOLUTION_EXPLICIT_SOCKET_UNREACHABLE,
    SOCKET_RESOLUTION_EXPLICIT_INSTANCE_UNREACHABLE,
    SOCKET_RESOLUTION_INVALID_INSTANCE_ID,
} SocketResolutionStatus;

static SocketResolutionStatus g_socket_resolution_status = SOCKET_RESOLUTION_NONE;
static char g_socket_resolution_detail[256];

static void
set_socket_resolution_status(SocketResolutionStatus status, const char *detail)
{
    g_socket_resolution_status = status;
    if (detail && detail[0])
        snprintf(g_socket_resolution_detail, sizeof(g_socket_resolution_detail),
                 "%s", detail);
    else
        g_socket_resolution_detail[0] = '\0';
}

static void
usage(void)
{
    fprintf(stderr,
        "Usage: prettymux-open <url>\n"
        "       prettymux-open --action <name> [--non-interactive]  Run action (hsplit, vsplit, etc.)\n"
        "       prettymux-open --exec <cmd>             Execute command in terminal\n"
        "       prettymux-open --type <text>            Type text into terminal\n"
        "       prettymux-open --new-workspace [name]   Create workspace\n"
        "       prettymux-open --new-tab                New terminal tab\n"
        "       prettymux-open --edit-tab               Start inline rename of focused tab\n"
        "       prettymux-open --edit-workspace [-w N]  Start inline rename of workspace N (or current)\n"
        "       prettymux-open --move-tab --from-w <n> --from-p <n> --from-t <n> --to-w <n> --to-p <n>\n"
        "       prettymux-open --move-workspace --to-instance <id> [-w <workspace-index>]\n"
        "       prettymux-open --select-tab -w <n> -p <n> -t <n>\n"
        "       prettymux-open --set-layout <mode> [-w N]  Set layout (classic/strip; omit -w to update all workspaces)\n"
        "       prettymux-open --get-layout [-w N]         Get layout mode\n"
        "       prettymux-open --get-strip-state [-w N]    Get strip columns/focus/maximize state\n"
        "       prettymux-open --set-workspace-status --id <id> --summary <text> [--provider <name>] [--kind <name>] [--state <state>] [--detail <text>] [--attention] [--notify] [-w N]\n"
        "       prettymux-open --clear-workspace-status --id <id> [-w N]\n"
        "       prettymux-open --list-workspace-status [-w N]\n"
        "       prettymux-open --list-workspaces        List workspaces\n"
        "       prettymux-open --list-tabs              List workspaces, panes, tabs\n"
        "       prettymux-open --list-actions           List all actions\n"
        "       prettymux-open --quit                   Close prettymux cleanly\n"
        "       prettymux-open --switch-workspace <n>   Switch to workspace N\n"
        "       prettymux-open --register-terminal <id> --session-id <sid>\n"
        "       prettymux-open --list-instances         List running instances\n"
        "       prettymux-open [--instance <id>] <command>\n"
        "       prettymux-open [--socket <path>] <command>\n"
        "\n"
        "Targeting (for --exec, --type):\n"
        "  -w <n>    Workspace index (0-based)\n"
        "  -p <n>    Pane index within workspace\n"
        "  -t <n>    Tab index within pane\n"
        "\n"
        "Action aliases:\n"
        "  hsplit          split.horizontal\n"
        "  vsplit          split.vertical\n"
        "  close           pane.close\n"
        "  zoom            pane.zoom\n"
        "  newtab          pane.tab.new\n"
        "  tabnext         tab.next\n"
        "  tabprev         tab.prev\n"
        "  palette         search.show\n"
        "  shortcuts       shortcuts.show\n"
        "  history         history.show\n"
        "  notes           notes.toggle\n"
        "  theme           theme.cycle\n"
        "  fullscreen      (F11)\n"
        "  broadcast       broadcast.toggle\n"
        "  search          terminal.search\n"
        "  copy            terminal.copy\n"
        "  paste           terminal.paste\n"
        "\n"
        "Set PRETTYMUX_SOCKET to the socket path.\n"
        "Set PRETTYMUX_INSTANCE_ID to prefer a specific instance by id.\n");
}

static int
socket_is_connectable(const char *path)
{
    int fd;
    struct sockaddr_un addr = {0};

    if (!path || !path[0])
        return 0;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int
instance_id_char_allowed(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.';
}

static int
instance_id_is_valid(const char *instance_id)
{
    if (!instance_id || !instance_id[0])
        return 0;

    for (const char *p = instance_id; *p; p++) {
        if (!instance_id_char_allowed(*p))
            return 0;
    }
    return 1;
}

static int
build_socket_path_for_instance(const char *instance_id, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !instance_id_is_valid(instance_id))
        return 0;
    snprintf(out, out_size, "/tmp/prettymux-%s.sock", instance_id);
    return 1;
}

static int
socket_name_matches_pattern(const char *name)
{
    size_t len;

    if (!name)
        return 0;
    len = strlen(name);
    if (len <= 15)
        return 0;
    if (strncmp(name, "prettymux-", 10) != 0)
        return 0;
    return strcmp(name + len - 5, ".sock") == 0;
}

static int
parse_instance_id_from_socket_name(const char *name, char *out, size_t out_size)
{
    size_t len;
    size_t id_len;

    if (!name || !out || out_size == 0 || !socket_name_matches_pattern(name))
        return 0;

    len = strlen(name);
    id_len = len - strlen("prettymux-") - strlen(".sock");
    if (id_len == 0 || id_len + 1 > out_size)
        return 0;

    memcpy(out, name + strlen("prettymux-"), id_len);
    out[id_len] = '\0';
    return instance_id_is_valid(out);
}

typedef struct {
    char instance_id[128];
    char socket_path[256];
} InstanceRecord;

static int
parse_numeric_instance_id(const char *instance_id, unsigned long long *value_out)
{
    char *end = NULL;
    unsigned long long value;

    if (!instance_id || !instance_id[0])
        return 0;

    errno = 0;
    value = strtoull(instance_id, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return 0;

    if (value_out)
        *value_out = value;
    return 1;
}

static int
compare_instance_id_for_default(const char *a, const char *b)
{
    unsigned long long a_num = 0;
    unsigned long long b_num = 0;
    int a_is_numeric = parse_numeric_instance_id(a, &a_num);
    int b_is_numeric = parse_numeric_instance_id(b, &b_num);

    if (a_is_numeric && b_is_numeric) {
        if (a_num < b_num)
            return -1;
        if (a_num > b_num)
            return 1;
    } else if (a_is_numeric != b_is_numeric) {
        return a_is_numeric ? -1 : 1;
    }

    return strcmp(a, b);
}

static int
compare_instance_record_for_default(const void *a, const void *b)
{
    const InstanceRecord *ia = a;
    const InstanceRecord *ib = b;

    return compare_instance_id_for_default(ia->instance_id, ib->instance_id);
}

static const char *
scan_default_running_socket(void)
{
    static char best_path[256];
    static char best_instance_id[128];
    DIR *d = NULL;
    struct dirent *ent;
    int have_best = 0;

    d = opendir("/tmp");
    if (!d)
        return NULL;

    while ((ent = readdir(d)) != NULL) {
        char candidate[256];
        char candidate_instance_id[128];
        size_t name_len = strlen(ent->d_name);

        if (!parse_instance_id_from_socket_name(ent->d_name,
                                                candidate_instance_id,
                                                sizeof(candidate_instance_id)))
            continue;
        if (name_len + 6 > sizeof(candidate))
            continue;

        memcpy(candidate, "/tmp/", 5);
        memcpy(candidate + 5, ent->d_name, name_len + 1);
        if (!socket_is_connectable(candidate))
            continue;

        if (!have_best || compare_instance_id_for_default(
                              candidate_instance_id, best_instance_id) < 0) {
            snprintf(best_path, sizeof(best_path), "%s", candidate);
            snprintf(best_instance_id, sizeof(best_instance_id), "%s",
                     candidate_instance_id);
            have_best = 1;
        }
    }

    closedir(d);
    return have_best ? best_path : NULL;
}

/* Find target socket with explicit target first, then env/default resolution. */
static const char *
find_socket(void)
{
    static char instance_socket[256];
    const char *env_socket;
    const char *env_instance;
    const char *scan_result;

    set_socket_resolution_status(SOCKET_RESOLUTION_NONE, NULL);

    if (g_cli_target_socket && g_cli_target_socket[0]) {
        if (socket_is_connectable(g_cli_target_socket))
            return g_cli_target_socket;
        set_socket_resolution_status(
            SOCKET_RESOLUTION_EXPLICIT_SOCKET_UNREACHABLE,
            g_cli_target_socket);
        return NULL;
    }

    if (g_cli_target_instance && g_cli_target_instance[0]) {
        if (!build_socket_path_for_instance(g_cli_target_instance,
                                            instance_socket,
                                            sizeof(instance_socket)))
        {
            set_socket_resolution_status(SOCKET_RESOLUTION_INVALID_INSTANCE_ID,
                                         g_cli_target_instance);
            return NULL;
        }
        if (socket_is_connectable(instance_socket))
            return instance_socket;
        set_socket_resolution_status(
            SOCKET_RESOLUTION_EXPLICIT_INSTANCE_UNREACHABLE,
            g_cli_target_instance);
        return NULL;
    }

    env_socket = getenv("PRETTYMUX_SOCKET");
    if (env_socket && env_socket[0]) {
        if (socket_is_connectable(env_socket))
            return env_socket;
        set_socket_resolution_status(
            SOCKET_RESOLUTION_EXPLICIT_SOCKET_UNREACHABLE,
            env_socket);
        return NULL;
    }

    env_instance = getenv("PRETTYMUX_INSTANCE_ID");
    if (env_instance && env_instance[0]) {
        if (!build_socket_path_for_instance(env_instance, instance_socket,
                                            sizeof(instance_socket))) {
            set_socket_resolution_status(SOCKET_RESOLUTION_INVALID_INSTANCE_ID,
                                         env_instance);
            return NULL;
        }
        if (socket_is_connectable(instance_socket))
            return instance_socket;
        set_socket_resolution_status(
            SOCKET_RESOLUTION_EXPLICIT_INSTANCE_UNREACHABLE,
            env_instance);
        return NULL;
    }

    scan_result = scan_default_running_socket();
    if (scan_result)
        return scan_result;

    set_socket_resolution_status(SOCKET_RESOLUTION_NO_RUNNING_INSTANCE, NULL);
    return NULL;
}

static int
write_all_bytes(int fd, const char *buf, size_t len)
{
    const char *cursor = buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (written == 0)
            return 0;

        cursor += written;
        remaining -= (size_t)written;
    }

    return 1;
}

static const char *
skip_ascii_space(const char *p)
{
    while (p && *p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int
response_status_exit_code(const char *text)
{
    const char *status_key;
    const char *value;

    if (!text)
        return 0;

    status_key = strstr(text, "\"status\"");
    if (!status_key)
        return 0;

    value = strchr(status_key, ':');
    if (!value)
        return 0;
    value = skip_ascii_space(value + 1);
    if (!value)
        return 0;

    if (strncmp(value, "\"ok\"", 4) == 0)
        return 0;
    if (strncmp(value, "\"error\"", 7) == 0)
        return 2;

    return 0;
}

static int
send_command(const char *json_msg)
{
    char *payload = NULL;
    const char *sock_path = find_socket();
    if (!sock_path) {
        if (g_cli_target_socket && g_cli_target_socket[0]) {
            fprintf(stderr,
                    "prettymux-open: target socket not reachable: %s\n",
                    g_cli_target_socket);
        } else if (g_cli_target_instance && g_cli_target_instance[0]) {
            fprintf(stderr,
                    "prettymux-open: target instance '%s' is not running.\n",
                    g_cli_target_instance);
        } else if (g_socket_resolution_status ==
                   SOCKET_RESOLUTION_EXPLICIT_SOCKET_UNREACHABLE) {
            fprintf(stderr,
                    "prettymux-open: PRETTYMUX_SOCKET target not reachable: %s\n",
                    g_socket_resolution_detail[0]
                        ? g_socket_resolution_detail
                        : "(unknown)");
        } else if (g_socket_resolution_status ==
                   SOCKET_RESOLUTION_EXPLICIT_INSTANCE_UNREACHABLE) {
            fprintf(stderr,
                    "prettymux-open: PRETTYMUX_INSTANCE_ID target '%s' is not running.\n",
                    g_socket_resolution_detail[0]
                        ? g_socket_resolution_detail
                        : "(unknown)");
        } else if (g_socket_resolution_status ==
                   SOCKET_RESOLUTION_INVALID_INSTANCE_ID) {
            fprintf(stderr,
                    "prettymux-open: invalid instance id in target: %s\n",
                    g_socket_resolution_detail[0]
                        ? g_socket_resolution_detail
                        : "(unknown)");
        } else {
            fprintf(stderr,
                "prettymux-open: no running prettymux instance found.\n"
                "Set PRETTYMUX_SOCKET/PRETTYMUX_INSTANCE_ID or start prettymux first.\n");
        }
        return 1;
    }

    if (g_cli_target_instance && g_cli_target_instance[0]) {
        size_t len = strlen(json_msg);
        char *escaped_instance = json_escape(g_cli_target_instance);
        size_t payload_len;

        if (!escaped_instance)
            return 1;

        payload_len = len + strlen(escaped_instance) + 32;
        payload = malloc(payload_len);
        if (!payload) {
            free(escaped_instance);
            return 1;
        }

        if (len >= 2 && json_msg[len - 1] == '}') {
            int has_members = !(len == 2 && json_msg[0] == '{');
            snprintf(payload, payload_len, "%.*s%s\"instanceId\":\"%s\"}",
                     (int)(len - 1), json_msg,
                     has_members ? "," : "",
                     escaped_instance);
        } else {
            snprintf(payload, payload_len, "%s", json_msg);
        }
        free(escaped_instance);
    } else {
        size_t len = strlen(json_msg);
        payload = malloc(len + 1);
        if (!payload)
            return 1;
        memcpy(payload, json_msg, len + 1);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("prettymux-open: socket");
        free(payload);
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("prettymux-open: connect");
        free(payload);
        close(fd);
        return 1;
    }

    if (!write_all_bytes(fd, payload, strlen(payload))) {
        perror("prettymux-open: write");
        free(payload);
        close(fd);
        return 1;
    }
    free(payload);

    shutdown(fd, SHUT_WR);

    char buf[8192];
    ssize_t total = 0, n;
    while ((n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0) {
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    buf[total] = '\0';
    close(fd);

    if (total > 0) {
        printf("%s", buf);
        if (buf[total - 1] != '\n') putchar('\n');
        return response_status_exit_code(buf);
    }
    return 0;
}

static char *
json_escape(const char *s)
{
    size_t len = strlen(s);
    char *out = malloc(len * 6 + 1);
    if (!out) return NULL;
    char *p = out;
    for (const char *c = s; *c; c++) {
        switch (*c) {
        case '"':  *p++ = '\\'; *p++ = '"'; break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        case '\t': *p++ = '\\'; *p++ = 't'; break;
        default:
            if ((unsigned char)*c < 0x20)
                p += sprintf(p, "\\u%04x", (unsigned char)*c);
            else
                *p++ = *c;
        }
    }
    *p = '\0';
    return out;
}

/* Resolve action aliases to full action names */
static const char *
resolve_alias(const char *name)
{
    static const struct { const char *alias; const char *action; } aliases[] = {
        {"hsplit",     "split.horizontal"},
        {"vsplit",     "split.vertical"},
        {"close",      "pane.close"},
        {"zoom",       "pane.zoom"},
        {"newtab",     "pane.tab.new"},
        {"tabnext",    "tab.next"},
        {"tabprev",    "tab.prev"},
        {"palette",    "search.show"},
        {"shortcuts",  "shortcuts.show"},
        {"history",    "history.show"},
        {"notes",      "notes.toggle"},
        {"theme",      "theme.cycle"},
        {"broadcast",  "broadcast.toggle"},
        {"search",     "terminal.search"},
        {"copy",       "terminal.copy"},
        {"paste",      "terminal.paste"},
        {NULL, NULL},
    };
    for (int i = 0; aliases[i].alias; i++) {
        if (strcmp(name, aliases[i].alias) == 0)
            return aliases[i].action;
    }
    return name; /* not an alias, use as-is */
}

/* Parse optional -w/-p/-t targeting flags from argv starting at *idx.
 * Advances *idx past consumed arguments. */
static void
parse_targeting(int argc, char *argv[], int *idx, int *ws, int *pane, int *tab)
{
    *ws = -1; *pane = -1; *tab = -1;
    while (*idx < argc) {
        if (strcmp(argv[*idx], "-w") == 0 && *idx + 1 < argc) {
            *ws = atoi(argv[++(*idx)]); (*idx)++;
        } else if (strcmp(argv[*idx], "-p") == 0 && *idx + 1 < argc) {
            *pane = atoi(argv[++(*idx)]); (*idx)++;
        } else if (strcmp(argv[*idx], "-t") == 0 && *idx + 1 < argc) {
            *tab = atoi(argv[++(*idx)]); (*idx)++;
        } else {
            break;
        }
    }
}

static int
list_running_instances(void)
{
    DIR *d;
    struct dirent *ent;
    InstanceRecord records[128];
    size_t count = 0;

    d = opendir("/tmp");
    if (!d) {
        fprintf(stderr, "prettymux-open: failed to open /tmp: %s\n",
                strerror(errno));
        return 1;
    }

    while ((ent = readdir(d)) != NULL && count < (sizeof(records) / sizeof(records[0]))) {
        char instance_id[128];
        size_t name_len = strlen(ent->d_name);

        if (!parse_instance_id_from_socket_name(ent->d_name,
                                                instance_id,
                                                sizeof(instance_id)))
            continue;
        if (name_len + 6 > sizeof(records[count].socket_path))
            continue;

        memcpy(records[count].socket_path, "/tmp/", 5);
        memcpy(records[count].socket_path + 5, ent->d_name, name_len + 1);
        if (!socket_is_connectable(records[count].socket_path))
            continue;

        snprintf(records[count].instance_id, sizeof(records[count].instance_id),
                 "%s", instance_id);
        count++;
    }
    closedir(d);

    qsort(records, count, sizeof(records[0]), compare_instance_record_for_default);

    printf("{\"status\":\"ok\",\"defaultInstanceId\":");
    if (count > 0)
        printf("\"%s\"", records[0].instance_id);
    else
        printf("null");

    printf(",\"instances\":[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            printf(",");
        printf("{\"instanceId\":\"%s\",\"socket\":\"%s\",\"default\":%s}",
               records[i].instance_id, records[i].socket_path,
               (i == 0) ? "true" : "false");
    }
    printf("]}\n");
    return 0;
}

int
main(int argc, char *argv[])
{
    int cmd_index = 1;
    int cmd_argc;
    char **cmd_argv;
    const char *arg;

    if (argc < 2) {
        usage();
        return 1;
    }

    while (cmd_index < argc) {
        if (strcmp(argv[cmd_index], "--instance") == 0) {
            if (cmd_index + 1 >= argc) {
                fprintf(stderr, "prettymux-open: --instance requires an id\n");
                return 1;
            }
            g_cli_target_instance = argv[cmd_index + 1];
            cmd_index += 2;
            continue;
        }
        if (strcmp(argv[cmd_index], "--socket") == 0) {
            if (cmd_index + 1 >= argc) {
                fprintf(stderr, "prettymux-open: --socket requires a path\n");
                return 1;
            }
            g_cli_target_socket = argv[cmd_index + 1];
            cmd_index += 2;
            continue;
        }
        break;
    }

    if (g_cli_target_instance && g_cli_target_socket) {
        fprintf(stderr,
                "prettymux-open: use either --instance or --socket, not both\n");
        return 1;
    }
    if (g_cli_target_instance && !instance_id_is_valid(g_cli_target_instance)) {
        fprintf(stderr, "prettymux-open: invalid instance id: %s\n",
                g_cli_target_instance);
        return 1;
    }

    if (cmd_index >= argc) {
        usage();
        return 1;
    }

    cmd_argc = argc - cmd_index;
    cmd_argv = argv + cmd_index;
    arg = cmd_argv[0];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
        usage();
        return 0;
    }

    if (strcmp(arg, "--list-instances") == 0)
        return list_running_instances();

    if (strcmp(arg, "--action") == 0) {
        int non_interactive = 0;

        if (cmd_argc < 2) {
            fprintf(stderr, "prettymux-open: --action requires a name\n");
            return 1;
        }
        for (int i = 2; i < cmd_argc; i++) {
            if (strcmp(cmd_argv[i], "--non-interactive") == 0) {
                non_interactive = 1;
            } else {
                fprintf(stderr,
                        "prettymux-open: unknown --action option: %s\n",
                        cmd_argv[i]);
                return 1;
            }
        }

        const char *action = resolve_alias(cmd_argv[1]);
        char *escaped = json_escape(action);
        char msg[512];
        if (non_interactive) {
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"action\",\"action\":\"%s\",\"nonInteractive\":true}",
                     escaped);
        } else {
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"action\",\"action\":\"%s\"}", escaped);
        }
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--exec") == 0) {
        if (cmd_argc < 2) {
            fprintf(stderr, "prettymux-open: --exec requires a command\n");
            return 1;
        }
        char *escaped = json_escape(cmd_argv[1]);
        int ws, pane, tab, idx = 2;
        parse_targeting(cmd_argc, cmd_argv, &idx, &ws, &pane, &tab);
        char msg[8192];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"exec\",\"cmd\":\"%s\",\"workspace\":%d,\"pane\":%d,\"tab\":%d}",
            escaped, ws, pane, tab);
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--type") == 0) {
        if (cmd_argc < 2) {
            fprintf(stderr, "prettymux-open: --type requires text\n");
            return 1;
        }
        char *escaped = json_escape(cmd_argv[1]);
        int ws, pane, tab, idx = 2;
        parse_targeting(cmd_argc, cmd_argv, &idx, &ws, &pane, &tab);
        char msg[8192];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"type\",\"text\":\"%s\",\"workspace\":%d,\"pane\":%d,\"tab\":%d}",
            escaped, ws, pane, tab);
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--new-workspace") == 0) {
        const char *name = (cmd_argc > 1) ? cmd_argv[1] : "";
        char *escaped = json_escape(name);
        char msg[512];
        snprintf(msg, sizeof(msg), "{\"command\":\"workspace.new\",\"name\":\"%s\"}", escaped);
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--new-tab") == 0) {
        return send_command("{\"command\":\"tab.new\"}");
    }

    if (strcmp(arg, "--edit-tab") == 0) {
        return send_command("{\"command\":\"tab.edit\"}");
    }

    if (strcmp(arg, "--edit-workspace") == 0) {
        int ws_idx = -1;
        for (int i = 1; i + 1 < cmd_argc; i += 2) {
            if (strcmp(cmd_argv[i], "-w") == 0)
                ws_idx = atoi(cmd_argv[i + 1]);
        }
        char msg[128];
        if (ws_idx >= 0)
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"workspace.edit\",\"workspace\":%d}",
                     ws_idx);
        else
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"workspace.edit\"}");
        return send_command(msg);
    }

    if (strcmp(arg, "--move-tab") == 0) {
        int from_w = -1, from_p = -1, from_t = -1, to_w = -1, to_p = -1;
        for (int i = 1; i + 1 < cmd_argc; i += 2) {
            if (strcmp(cmd_argv[i], "--from-w") == 0) from_w = atoi(cmd_argv[i + 1]);
            else if (strcmp(cmd_argv[i], "--from-p") == 0) from_p = atoi(cmd_argv[i + 1]);
            else if (strcmp(cmd_argv[i], "--from-t") == 0) from_t = atoi(cmd_argv[i + 1]);
            else if (strcmp(cmd_argv[i], "--to-w") == 0) to_w = atoi(cmd_argv[i + 1]);
            else if (strcmp(cmd_argv[i], "--to-p") == 0) to_p = atoi(cmd_argv[i + 1]);
        }
        if (from_w < 0 || from_p < 0 || from_t < 0 || to_w < 0 || to_p < 0) {
            fprintf(stderr, "prettymux-open: --move-tab requires --from-w --from-p --from-t --to-w --to-p\n");
            return 1;
        }
        char msg[256];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"tab.move\",\"fromWorkspace\":%d,\"fromPane\":%d,\"fromTab\":%d,\"toWorkspace\":%d,\"toPane\":%d}",
            from_w, from_p, from_t, to_w, to_p);
        return send_command(msg);
    }

    if (strcmp(arg, "--move-workspace") == 0) {
        const char *target_instance = NULL;
        int ws_idx = -1;

        for (int i = 1; i < cmd_argc; i++) {
            if (strcmp(cmd_argv[i], "--to-instance") == 0 && i + 1 < cmd_argc) {
                target_instance = cmd_argv[++i];
            } else if (strcmp(cmd_argv[i], "-w") == 0 && i + 1 < cmd_argc) {
                ws_idx = atoi(cmd_argv[++i]);
            } else {
                fprintf(stderr,
                        "prettymux-open: unknown --move-workspace option: %s\n",
                        cmd_argv[i]);
                return 1;
            }
        }

        if (!target_instance || !target_instance[0]) {
            fprintf(stderr,
                    "prettymux-open: --move-workspace requires --to-instance <id>\n");
            return 1;
        }
        if (!instance_id_is_valid(target_instance)) {
            fprintf(stderr, "prettymux-open: invalid target instance id: %s\n",
                    target_instance);
            return 1;
        }

        {
            char *escaped_target = json_escape(target_instance);
            char msg[1024];

            if (!escaped_target)
                return 1;

            if (ws_idx >= 0) {
                snprintf(msg, sizeof(msg),
                         "{\"command\":\"workspace.move_to_instance\",\"workspace\":%d,\"targetInstanceId\":\"%s\"}",
                         ws_idx, escaped_target);
            } else {
                snprintf(msg, sizeof(msg),
                         "{\"command\":\"workspace.move_to_instance\",\"targetInstanceId\":\"%s\"}",
                         escaped_target);
            }
            free(escaped_target);
            return send_command(msg);
        }
    }

    if (strcmp(arg, "--select-tab") == 0) {
        int ws, pane, tab, idx = 1;
        parse_targeting(cmd_argc, cmd_argv, &idx, &ws, &pane, &tab);
        if (ws < 0 || pane < 0 || tab < 0) {
            fprintf(stderr, "prettymux-open: --select-tab requires -w -p -t\n");
            return 1;
        }
        char msg[256];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"tab.select\",\"workspace\":%d,\"pane\":%d,\"tab\":%d}",
            ws, pane, tab);
        return send_command(msg);
    }

    if (strcmp(arg, "--set-layout") == 0) {
        if (cmd_argc < 2) {
            fprintf(stderr, "prettymux-open: --set-layout requires a mode (classic/strip)\n");
            return 1;
        }
        const char *layout = cmd_argv[1];
        int ws_idx = -1, dummy_p, dummy_t, idx = 2;
        parse_targeting(cmd_argc, cmd_argv, &idx, &ws_idx, &dummy_p, &dummy_t);
        char *escaped = json_escape(layout);
        char msg[512];
        if (ws_idx >= 0)
            snprintf(msg, sizeof(msg),
                "{\"command\":\"workspace.set_layout\",\"layout\":\"%s\",\"workspace\":%d}",
                escaped, ws_idx);
        else
            snprintf(msg, sizeof(msg),
                "{\"command\":\"workspace.set_layout\",\"layout\":\"%s\"}",
                escaped);
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--get-layout") == 0) {
        int ws_idx = -1, dummy_p, dummy_t, idx = 1;
        parse_targeting(cmd_argc, cmd_argv, &idx, &ws_idx, &dummy_p, &dummy_t);
        char msg[256];
        if (ws_idx >= 0)
            snprintf(msg, sizeof(msg),
                "{\"command\":\"workspace.get_layout\",\"workspace\":%d}", ws_idx);
        else
            snprintf(msg, sizeof(msg), "{\"command\":\"workspace.get_layout\"}");
        return send_command(msg);
    }

    if (strcmp(arg, "--get-strip-state") == 0) {
        int ws_idx = -1, dummy_p, dummy_t, idx = 1;
        parse_targeting(cmd_argc, cmd_argv, &idx, &ws_idx, &dummy_p, &dummy_t);
        char msg[256];
        if (ws_idx >= 0)
            snprintf(msg, sizeof(msg),
                "{\"command\":\"workspace.get_strip_state\",\"workspace\":%d}", ws_idx);
        else
            snprintf(msg, sizeof(msg), "{\"command\":\"workspace.get_strip_state\"}");
        return send_command(msg);
    }

    if (strcmp(arg, "--set-workspace-status") == 0) {
        const char *entry_id = NULL;
        const char *provider = "";
        const char *kind = "";
        const char *state = "";
        const char *summary = "";
        const char *detail = "";
        int ws_idx = -1;
        int attention = 0;
        int notify = 0;

        for (int i = 1; i < cmd_argc; i++) {
            if (strcmp(cmd_argv[i], "--id") == 0 && i + 1 < cmd_argc) {
                entry_id = cmd_argv[++i];
            } else if (strcmp(cmd_argv[i], "--provider") == 0 &&
                       i + 1 < cmd_argc) {
                provider = cmd_argv[++i];
            } else if (strcmp(cmd_argv[i], "--kind") == 0 &&
                       i + 1 < cmd_argc) {
                kind = cmd_argv[++i];
            } else if (strcmp(cmd_argv[i], "--state") == 0 &&
                       i + 1 < cmd_argc) {
                state = cmd_argv[++i];
            } else if (strcmp(cmd_argv[i], "--summary") == 0 &&
                       i + 1 < cmd_argc) {
                summary = cmd_argv[++i];
            } else if (strcmp(cmd_argv[i], "--detail") == 0 &&
                       i + 1 < cmd_argc) {
                detail = cmd_argv[++i];
            } else if (strcmp(cmd_argv[i], "--attention") == 0) {
                attention = 1;
            } else if (strcmp(cmd_argv[i], "--notify") == 0) {
                notify = 1;
            } else if (strcmp(cmd_argv[i], "-w") == 0 && i + 1 < cmd_argc) {
                ws_idx = atoi(cmd_argv[++i]);
            } else {
                fprintf(stderr, "prettymux-open: unknown --set-workspace-status option: %s\n",
                        cmd_argv[i]);
                return 1;
            }
        }

        if (!entry_id || !entry_id[0]) {
            fprintf(stderr,
                    "prettymux-open: --set-workspace-status requires --id <id>\n");
            return 1;
        }
        if ((!summary || !summary[0]) && (!state || !state[0]) &&
            (!detail || !detail[0])) {
            fprintf(stderr,
                    "prettymux-open: --set-workspace-status requires --summary, --state, or --detail\n");
            return 1;
        }

        char *escaped_id = json_escape(entry_id);
        char *escaped_provider = json_escape(provider);
        char *escaped_kind = json_escape(kind);
        char *escaped_state = json_escape(state);
        char *escaped_summary = json_escape(summary);
        char *escaped_detail = json_escape(detail);
        char msg[16384];
        if (ws_idx >= 0) {
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"workspace.status.set\",\"entryId\":\"%s\",\"provider\":\"%s\",\"kind\":\"%s\",\"state\":\"%s\",\"summary\":\"%s\",\"detail\":\"%s\",\"attention\":%s,\"notify\":%s,\"workspace\":%d}",
                     escaped_id, escaped_provider, escaped_kind, escaped_state,
                     escaped_summary, escaped_detail,
                     attention ? "true" : "false",
                     notify ? "true" : "false",
                     ws_idx);
        } else {
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"workspace.status.set\",\"entryId\":\"%s\",\"provider\":\"%s\",\"kind\":\"%s\",\"state\":\"%s\",\"summary\":\"%s\",\"detail\":\"%s\",\"attention\":%s,\"notify\":%s}",
                     escaped_id, escaped_provider, escaped_kind, escaped_state,
                     escaped_summary, escaped_detail,
                     attention ? "true" : "false",
                     notify ? "true" : "false");
        }

        free(escaped_id);
        free(escaped_provider);
        free(escaped_kind);
        free(escaped_state);
        free(escaped_summary);
        free(escaped_detail);
        return send_command(msg);
    }

    if (strcmp(arg, "--clear-workspace-status") == 0) {
        const char *entry_id = NULL;
        int ws_idx = -1;

        for (int i = 1; i < cmd_argc; i++) {
            if (strcmp(cmd_argv[i], "--id") == 0 && i + 1 < cmd_argc) {
                entry_id = cmd_argv[++i];
            } else if (strcmp(cmd_argv[i], "-w") == 0 && i + 1 < cmd_argc) {
                ws_idx = atoi(cmd_argv[++i]);
            } else {
                fprintf(stderr, "prettymux-open: unknown --clear-workspace-status option: %s\n",
                        cmd_argv[i]);
                return 1;
            }
        }

        if (!entry_id || !entry_id[0]) {
            fprintf(stderr,
                    "prettymux-open: --clear-workspace-status requires --id <id>\n");
            return 1;
        }

        char *escaped_id = json_escape(entry_id);
        char msg[1024];
        if (ws_idx >= 0) {
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"workspace.status.clear\",\"entryId\":\"%s\",\"workspace\":%d}",
                     escaped_id, ws_idx);
        } else {
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"workspace.status.clear\",\"entryId\":\"%s\"}",
                     escaped_id);
        }
        free(escaped_id);
        return send_command(msg);
    }

    if (strcmp(arg, "--list-workspace-status") == 0) {
        int ws_idx = -1;

        for (int i = 1; i < cmd_argc; i++) {
            if (strcmp(cmd_argv[i], "-w") == 0 && i + 1 < cmd_argc) {
                ws_idx = atoi(cmd_argv[++i]);
            } else {
                fprintf(stderr, "prettymux-open: unknown --list-workspace-status option: %s\n",
                        cmd_argv[i]);
                return 1;
            }
        }

        char msg[256];
        if (ws_idx >= 0) {
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"workspace.status.list\",\"workspace\":%d}",
                     ws_idx);
        } else {
            snprintf(msg, sizeof(msg),
                     "{\"command\":\"workspace.status.list\"}");
        }
        return send_command(msg);
    }

    if (strcmp(arg, "--list-workspaces") == 0) {
        return send_command("{\"command\":\"workspace.list\"}");
    }

    if (strcmp(arg, "--list-tabs") == 0) {
        return send_command("{\"command\":\"tabs.list\"}");
    }

    if (strcmp(arg, "--list-actions") == 0) {
        return send_command("{\"command\":\"list.actions\"}");
    }

    if (strcmp(arg, "--quit") == 0) {
        return send_command("{\"command\":\"app.quit\"}");
    }

    if (strcmp(arg, "--switch-workspace") == 0) {
        if (cmd_argc < 2) {
            fprintf(stderr, "prettymux-open: --switch-workspace requires index\n");
            return 1;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "{\"command\":\"workspace.switch\",\"index\":%d}",
                 atoi(cmd_argv[1]));
        return send_command(msg);
    }

    if (strcmp(arg, "--register-terminal") == 0) {
        const char *terminal_id = NULL;
        const char *tty_name = "";
        const char *tty_path = "";
        int session_id = 0;

        if (cmd_argc < 4) {
            fprintf(stderr,
                    "prettymux-open: --register-terminal requires <id> --session-id <sid>\n");
            return 1;
        }

        terminal_id = cmd_argv[1];
        for (int i = 2; i + 1 < cmd_argc; i += 2) {
            if (strcmp(cmd_argv[i], "--session-id") == 0)
                session_id = atoi(cmd_argv[i + 1]);
            else if (strcmp(cmd_argv[i], "--tty-name") == 0)
                tty_name = cmd_argv[i + 1];
            else if (strcmp(cmd_argv[i], "--tty-path") == 0)
                tty_path = cmd_argv[i + 1];
        }

        if (!terminal_id || !terminal_id[0] || session_id <= 0) {
            fprintf(stderr,
                    "prettymux-open: --register-terminal requires <id> --session-id <sid>\n");
            return 1;
        }

        char *escaped_id = json_escape(terminal_id);
        char *escaped_tty_name = json_escape(tty_name);
        char *escaped_tty_path = json_escape(tty_path);
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "{\"command\":\"terminal.register\",\"terminalId\":\"%s\",\"sessionId\":%d,\"ttyName\":\"%s\",\"ttyPath\":\"%s\"}",
                 escaped_id, session_id, escaped_tty_name, escaped_tty_path);
        free(escaped_id);
        free(escaped_tty_name);
        free(escaped_tty_path);
        return send_command(msg);
    }

    /* Default: hand URLs to the system default handler. */
    {
        char *escaped = json_escape(cmd_argv[0]);
        char msg[4096];
        snprintf(msg, sizeof(msg),
                 "{\"command\":\"open.url\",\"url\":\"%s\"}", escaped);
        free(escaped);
        return send_command(msg);
    }
}
