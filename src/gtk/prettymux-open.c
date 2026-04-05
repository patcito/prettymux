/*
 * prettymux-open.c - CLI tool for controlling a running prettymux instance
 *
 * Communicates with prettymux via a Unix domain socket (path from
 * PRETTYMUX_SOCKET env var).
 *
 * Usage:
 *   prettymux-open <url>                          Open URL in browser tab
 *   prettymux-open --action <name>                Run any action (e.g. split.horizontal)
 *   prettymux-open --exec <cmd>                   Execute command in focused terminal
 *   prettymux-open --type <text>                  Type text into focused terminal
 *   prettymux-open --exec <cmd> -w 0 -p 1 -t 0   Target specific workspace/pane/tab
 *   prettymux-open --new-workspace [name]         Create a new workspace
 *   prettymux-open --new-tab                      Create a new terminal tab
 *   prettymux-open --move-tab ...                 Move a terminal tab to another pane
 *   prettymux-open --select-tab -w 0 -p 1 -t 0    Select a specific terminal tab
 *   prettymux-open --list-workspaces              List all workspaces
 *   prettymux-open --list-tabs                    List workspaces/panes/tabs
 *   prettymux-open --list-actions                 List all available actions
 *   prettymux-open --quit                         Close prettymux cleanly
 *   prettymux-open --switch-workspace <n>         Switch to workspace N
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>

static void
usage(void)
{
    fprintf(stderr,
        "Usage: prettymux-open <url>\n"
        "       prettymux-open --action <name>          Run action (hsplit, vsplit, etc.)\n"
        "       prettymux-open --exec <cmd>             Execute command in terminal\n"
        "       prettymux-open --type <text>            Type text into terminal\n"
        "       prettymux-open --new-workspace [name]   Create workspace\n"
        "       prettymux-open --new-tab                New terminal tab\n"
        "       prettymux-open --move-tab --from-w <n> --from-p <n> --from-t <n> --to-w <n> --to-p <n>\n"
        "       prettymux-open --select-tab -w <n> -p <n> -t <n>\n"
        "       prettymux-open --list-workspaces        List workspaces\n"
        "       prettymux-open --list-tabs              List workspaces, panes, tabs\n"
        "       prettymux-open --list-actions           List all actions\n"
        "       prettymux-open --quit                   Close prettymux cleanly\n"
        "       prettymux-open --switch-workspace <n>   Switch to workspace N\n"
        "       prettymux-open --register-terminal <id> --session-id <sid> [--tty-name <tty>] [--tty-path <path>]\n"
        "       prettymux-open --report-port <port> --terminal-id <id>\n"
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
        "  browser         browser.toggle\n"
        "  palette         search.show\n"
        "  shortcuts       shortcuts.show\n"
        "  history         history.show\n"
        "  notes           notes.toggle\n"
        "  pip             pip.toggle\n"
        "  theme           theme.cycle\n"
        "  fullscreen      (F11)\n"
        "  broadcast       broadcast.toggle\n"
        "  search          terminal.search\n"
        "  copy            terminal.copy\n"
        "  paste           terminal.paste\n"
        "\n"
        "Set PRETTYMUX_SOCKET to the socket path.\n");
}

/* Find the prettymux socket: check env var first, then scan /tmp */
static const char *
find_socket(void)
{
    const char *env = getenv("PRETTYMUX_SOCKET");
    if (env && env[0]) return env;

    /* Scan /tmp for prettymux-*.sock */
    static char found[256];
    DIR *d = opendir("/tmp");
    if (!d) return NULL;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "prettymux-", 10) == 0 &&
            strstr(ent->d_name, ".sock") != NULL) {
            snprintf(found, sizeof(found), "/tmp/%s", ent->d_name);
            /* Check if socket is connectable */
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct sockaddr_un addr = {0};
                addr.sun_family = AF_UNIX;
                snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", found);
                if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    close(fd);
                    closedir(d);
                    return found;
                }
                close(fd);
            }
        }
    }
    closedir(d);
    return NULL;
}

static int
send_command(const char *json_msg)
{
    const char *sock_path = find_socket();
    if (!sock_path) {
        fprintf(stderr,
            "prettymux-open: no running prettymux instance found.\n"
            "Set PRETTYMUX_SOCKET or start prettymux first.\n");
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("prettymux-open: socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("prettymux-open: connect");
        close(fd);
        return 1;
    }

    ssize_t written = write(fd, json_msg, strlen(json_msg));
    if (written < 0) { perror("prettymux-open: write"); close(fd); return 1; }

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
        {"browser",    "browser.toggle"},
        {"palette",    "search.show"},
        {"shortcuts",  "shortcuts.show"},
        {"history",    "history.show"},
        {"notes",      "notes.toggle"},
        {"pip",        "pip.toggle"},
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

int
main(int argc, char *argv[])
{
    if (argc < 2) { usage(); return 1; }

    const char *arg = argv[1];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
        usage(); return 0;
    }

    if (strcmp(arg, "--action") == 0) {
        if (argc < 3) { fprintf(stderr, "prettymux-open: --action requires a name\n"); return 1; }
        const char *action = resolve_alias(argv[2]);
        char *escaped = json_escape(action);
        char msg[512];
        snprintf(msg, sizeof(msg), "{\"command\":\"action\",\"action\":\"%s\"}", escaped);
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--exec") == 0) {
        if (argc < 3) { fprintf(stderr, "prettymux-open: --exec requires a command\n"); return 1; }
        char *escaped = json_escape(argv[2]);
        int ws, pane, tab, idx = 3;
        parse_targeting(argc, argv, &idx, &ws, &pane, &tab);
        char msg[8192];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"exec\",\"cmd\":\"%s\",\"workspace\":%d,\"pane\":%d,\"tab\":%d}",
            escaped, ws, pane, tab);
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--type") == 0) {
        if (argc < 3) { fprintf(stderr, "prettymux-open: --type requires text\n"); return 1; }
        char *escaped = json_escape(argv[2]);
        int ws, pane, tab, idx = 3;
        parse_targeting(argc, argv, &idx, &ws, &pane, &tab);
        char msg[8192];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"type\",\"text\":\"%s\",\"workspace\":%d,\"pane\":%d,\"tab\":%d}",
            escaped, ws, pane, tab);
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--new-workspace") == 0) {
        const char *name = (argc > 2) ? argv[2] : "";
        char *escaped = json_escape(name);
        char msg[512];
        snprintf(msg, sizeof(msg), "{\"command\":\"workspace.new\",\"name\":\"%s\"}", escaped);
        free(escaped);
        return send_command(msg);
    }

    if (strcmp(arg, "--new-tab") == 0) {
        return send_command("{\"command\":\"tab.new\"}");
    }

    if (strcmp(arg, "--move-tab") == 0) {
        int from_w = -1, from_p = -1, from_t = -1, to_w = -1, to_p = -1;
        for (int i = 2; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--from-w") == 0) from_w = atoi(argv[i + 1]);
            else if (strcmp(argv[i], "--from-p") == 0) from_p = atoi(argv[i + 1]);
            else if (strcmp(argv[i], "--from-t") == 0) from_t = atoi(argv[i + 1]);
            else if (strcmp(argv[i], "--to-w") == 0) to_w = atoi(argv[i + 1]);
            else if (strcmp(argv[i], "--to-p") == 0) to_p = atoi(argv[i + 1]);
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

    if (strcmp(arg, "--select-tab") == 0) {
        int ws, pane, tab, idx = 2;
        parse_targeting(argc, argv, &idx, &ws, &pane, &tab);
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
        if (argc < 3) { fprintf(stderr, "prettymux-open: --switch-workspace requires index\n"); return 1; }
        char msg[128];
        snprintf(msg, sizeof(msg), "{\"command\":\"workspace.switch\",\"index\":%d}", atoi(argv[2]));
        return send_command(msg);
    }

    if (strcmp(arg, "--register-terminal") == 0) {
        const char *terminal_id = NULL;
        const char *tty_name = "";
        const char *tty_path = "";
        int session_id = 0;

        if (argc < 5) {
            fprintf(stderr,
                    "prettymux-open: --register-terminal requires <id> --session-id <sid>\n");
            return 1;
        }

        terminal_id = argv[2];
        for (int i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--session-id") == 0)
                session_id = atoi(argv[i + 1]);
            else if (strcmp(argv[i], "--tty-name") == 0)
                tty_name = argv[i + 1];
            else if (strcmp(argv[i], "--tty-path") == 0)
                tty_path = argv[i + 1];
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

    if (strcmp(arg, "--report-port") == 0) {
        if (argc < 5) {
            fprintf(stderr,
                    "prettymux-open: --report-port requires <port> --terminal-id <id>\n");
            return 1;
        }

        int port = atoi(argv[2]);
        const char *terminal_id = NULL;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--terminal-id") == 0) {
                terminal_id = argv[i + 1];
                break;
            }
        }

        if (port <= 0 || !terminal_id || !terminal_id[0]) {
            fprintf(stderr,
                    "prettymux-open: --report-port requires <port> --terminal-id <id>\n");
            return 1;
        }

        char *escaped = json_escape(terminal_id);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "{\"command\":\"port.report\",\"port\":%d,\"terminalId\":\"%s\"}",
                 port, escaped);
        free(escaped);
        return send_command(msg);
    }

    /* Default: treat argument as URL */
    {
        char *escaped = json_escape(arg);
        char msg[4096];
        snprintf(msg, sizeof(msg), "{\"command\":\"browser.open\",\"url\":\"%s\"}", escaped);
        free(escaped);
        return send_command(msg);
    }
}
