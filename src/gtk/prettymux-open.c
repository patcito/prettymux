/*
 * prettymux-open.c - CLI tool for controlling a running prettymux instance
 *
 * Communicates with prettymux via a Unix domain socket (path from
 * PRETTYMUX_SOCKET env var).
 *
 * Usage:
 *   prettymux-open <url>                     Open URL in browser tab
 *   prettymux-open --new-workspace [name]    Create a new workspace
 *   prettymux-open --new-tab                 Create a new terminal tab
 *   prettymux-open --list-workspaces         List all workspaces
 *   prettymux-open --switch-workspace <n>    Switch to workspace N
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void
usage(void)
{
    fprintf(stderr,
        "Usage: prettymux-open <url>\n"
        "       prettymux-open --new-workspace [name]\n"
        "       prettymux-open --new-tab\n"
        "       prettymux-open --list-workspaces\n"
        "       prettymux-open --switch-workspace <n>\n"
        "\n"
        "Controls a running prettymux instance via its socket.\n"
        "Set PRETTYMUX_SOCKET to the socket path.\n");
}

/*
 * Connect to the prettymux socket, send a JSON message, read and print
 * the JSON response.  Returns 0 on success, 1 on error.
 */
static int
send_command(const char *json_msg)
{
    const char *sock_path = getenv("PRETTYMUX_SOCKET");
    if (!sock_path || sock_path[0] == '\0') {
        fprintf(stderr,
            "prettymux-open: PRETTYMUX_SOCKET not set.\n"
            "Are you running inside a prettymux terminal?\n");
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("prettymux-open: socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("prettymux-open: connect");
        close(fd);
        return 1;
    }

    /* Send the JSON command */
    size_t msg_len = strlen(json_msg);
    ssize_t written = write(fd, json_msg, msg_len);
    if (written < 0) {
        perror("prettymux-open: write");
        close(fd);
        return 1;
    }

    /* Shutdown write side so the server knows we're done sending */
    shutdown(fd, SHUT_WR);

    /* Read the response */
    char buf[8192];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total,
                     sizeof(buf) - 1 - (size_t)total)) > 0) {
        total += n;
        if ((size_t)total >= sizeof(buf) - 1)
            break;
    }
    buf[total] = '\0';

    close(fd);

    if (total > 0) {
        printf("%s", buf);
        if (buf[total - 1] != '\n')
            putchar('\n');
    }

    return 0;
}

/*
 * Escape a string for inclusion in a JSON string value.
 * The caller must free the returned buffer.
 */
static char *
json_escape(const char *s)
{
    size_t len = strlen(s);
    /* Worst case: every char needs \uXXXX (6 chars) */
    char *out = malloc(len * 6 + 1);
    if (!out) return NULL;

    char *p = out;
    const char *c;
    for (c = s; *c; c++) {
        switch (*c) {
        case '"':  *p++ = '\\'; *p++ = '"'; break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        case '\t': *p++ = '\\'; *p++ = 't'; break;
        default:
            if ((unsigned char)*c < 0x20) {
                p += sprintf(p, "\\u%04x", (unsigned char)*c);
            } else {
                *p++ = *c;
            }
        }
    }
    *p = '\0';
    return out;
}

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *arg = argv[1];

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
        usage();
        return 0;
    }

    if (strcmp(arg, "--new-workspace") == 0) {
        const char *name = (argc > 2) ? argv[2] : "";
        char *escaped_name = json_escape(name);
        char msg[512];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"workspace.new\",\"name\":\"%s\"}",
            escaped_name);
        free(escaped_name);
        return send_command(msg);
    }

    if (strcmp(arg, "--new-tab") == 0) {
        return send_command("{\"command\":\"tab.new\"}");
    }

    if (strcmp(arg, "--list-workspaces") == 0) {
        return send_command("{\"command\":\"workspace.list\"}");
    }

    if (strcmp(arg, "--switch-workspace") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                "prettymux-open: --switch-workspace requires an index\n");
            return 1;
        }
        int idx = atoi(argv[2]);
        char msg[128];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"workspace.switch\",\"index\":%d}", idx);
        return send_command(msg);
    }

    /* Default: treat the argument as a URL to open */
    {
        char *escaped_url = json_escape(arg);
        char msg[4096];
        snprintf(msg, sizeof(msg),
            "{\"command\":\"browser.open\",\"url\":\"%s\"}",
            escaped_url);
        free(escaped_url);
        return send_command(msg);
    }
}
