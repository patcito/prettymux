#define _GNU_SOURCE
/*
 * port_scanner.c - Centralized listening-port tracking for PrettyMux
 *
 * A single app-owned scanner polls Linux socket diagnostics and attributes
 * newly-listening TCP ports to registered PrettyMux terminals by matching
 * process session IDs (with TTY fallback metadata retained for future use).
 */

#include "port_scanner.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <arpa/inet.h>
#include <dirent.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

typedef struct {
    char *terminal_id;
    pid_t session_id;
    char *tty_name;
    char *tty_path;
    int workspace_idx;
} TerminalScope;

static guint scan_timer_id = 0;
static PortScannerCallback scan_callback = NULL;
static gpointer scan_user_data = NULL;
static GHashTable *terminal_scopes = NULL; /* terminal_id -> TerminalScope* */
static GHashTable *known_port_keys = NULL; /* "terminal|port" -> TRUE */
static int active_workspace_idx = -1;
static gboolean window_active = TRUE;
static gint64 last_background_scan_us = 0;

#ifdef __linux__
static const gint64 SLOW_SCAN_INTERVAL_US = 300 * G_USEC_PER_SEC;
#ifndef TCP_LISTEN
#define TCP_LISTEN 10
#endif
#endif

static void
terminal_scope_free(gpointer data)
{
    TerminalScope *scope = data;

    if (!scope)
        return;

    g_free(scope->terminal_id);
    g_free(scope->tty_name);
    g_free(scope->tty_path);
    g_free(scope);
}

static gboolean
is_system_service_port(int port)
{
    if (port == 5432 || port == 5433 || port == 5434)
        return TRUE;
    if (port == 6379 || port == 6380)
        return TRUE;
    if (port == 11434)
        return TRUE;
    if (port == 4723 || port == 5037 || port == 5054)
        return TRUE;
    if (port >= 35000)
        return TRUE;
    return FALSE;
}

static gboolean
should_track_port(int port)
{
    if (port < 1024)
        return FALSE;
    if (is_system_service_port(port))
        return FALSE;
    return TRUE;
}

static char *
port_key_new(const char *terminal_id, int port)
{
    return g_strdup_printf("%s|%d", terminal_id, port);
}

static gboolean
key_matches_any_scanned_terminal(gpointer key, gpointer value, gpointer user_data)
{
    const char *port_key = key;
    GHashTable *scanned_ids = user_data;
    const char *sep;
    char *terminal_id;
    gboolean remove;

    (void)value;

    sep = strrchr(port_key, '|');
    if (!sep)
        return FALSE;

    terminal_id = g_strndup(port_key, sep - port_key);
    remove = g_hash_table_contains(scanned_ids, terminal_id);
    g_free(terminal_id);
    return remove;
}

#ifdef __linux__
static void
collect_diag_family(int family, GHashTable *inode_to_port)
{
    int fd;
    struct {
        struct nlmsghdr nlh;
        struct inet_diag_req_v2 req;
    } request;
    char buffer[8192];
    ssize_t received;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0)
        return;

    memset(&request, 0, sizeof(request));
    request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(request.req));
    request.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.req.sdiag_family = family;
    request.req.sdiag_protocol = IPPROTO_TCP;
    request.req.idiag_states = (1U << TCP_LISTEN);
    request.req.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
    request.req.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;

    if (send(fd, &request, request.nlh.nlmsg_len, 0) < 0) {
        close(fd);
        return;
    }

    for (;;) {
        struct nlmsghdr *hdr;

        received = recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;

        for (hdr = (struct nlmsghdr *)buffer;
             NLMSG_OK(hdr, (unsigned int)received);
             hdr = NLMSG_NEXT(hdr, received)) {
            if (hdr->nlmsg_type == NLMSG_DONE) {
                close(fd);
                return;
            }

            if (hdr->nlmsg_type == NLMSG_ERROR) {
                close(fd);
                return;
            }

            if (hdr->nlmsg_len < NLMSG_LENGTH(sizeof(struct inet_diag_msg)))
                continue;

            struct inet_diag_msg *diag = NLMSG_DATA(hdr);
            int port = (int)ntohs(diag->id.idiag_sport);

            if (!should_track_port(port))
                continue;

            g_hash_table_insert(inode_to_port,
                                GUINT_TO_POINTER(diag->idiag_inode),
                                GINT_TO_POINTER(port));
        }
    }

    close(fd);
}

static GHashTable *
collect_listening_socket_inodes(void)
{
    GHashTable *inode_to_port;

    inode_to_port = g_hash_table_new(g_direct_hash, g_direct_equal);
    collect_diag_family(AF_INET, inode_to_port);
    collect_diag_family(AF_INET6, inode_to_port);
    return inode_to_port;
}

static gboolean
scope_matches_pid(TerminalScope *scope, pid_t pid)
{
    pid_t sid;

    if (!scope)
        return FALSE;

    if (scope->session_id > 0) {
        sid = getsid(pid);
        if (sid > 0 && sid == scope->session_id)
            return TRUE;
    }

    return FALSE;
}

static void
scan_pid_fds_for_scope(pid_t pid,
                       GPtrArray *matching_scopes,
                       GHashTable *inode_to_port,
                       GHashTable *current_keys)
{
    char fd_dir_path[64];
    DIR *fd_dir;
    struct dirent *ent;

    if (!matching_scopes || matching_scopes->len == 0)
        return;

    snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%ld/fd", (long)pid);
    fd_dir = opendir(fd_dir_path);
    if (!fd_dir)
        return;

    while ((ent = readdir(fd_dir)) != NULL) {
        char link_path[96];
        char target[256];
        ssize_t len;
        unsigned int inode = 0;
        gpointer port_ptr;
        int port;

        if (ent->d_name[0] == '.')
            continue;

        snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir_path, ent->d_name);
        len = readlink(link_path, target, sizeof(target) - 1);
        if (len <= 0)
            continue;

        target[len] = '\0';
        if (sscanf(target, "socket:[%u]", &inode) != 1)
            continue;

        port_ptr = g_hash_table_lookup(inode_to_port, GUINT_TO_POINTER(inode));
        if (!port_ptr)
            continue;

        port = GPOINTER_TO_INT(port_ptr);
        for (guint i = 0; i < matching_scopes->len; i++) {
            TerminalScope *scope = g_ptr_array_index(matching_scopes, i);
            char *key = port_key_new(scope->terminal_id, port);
            g_hash_table_add(current_keys, key);
        }
    }

    closedir(fd_dir);
}

static GHashTable *
collect_current_port_keys(GHashTable *scanned_ids)
{
    GHashTable *current_keys;
    GHashTable *inode_to_port;
    GPtrArray *scopes;
    DIR *proc_dir;
    struct dirent *ent;

    current_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!terminal_scopes || g_hash_table_size(scanned_ids) == 0)
        return current_keys;

    inode_to_port = collect_listening_socket_inodes();
    if (g_hash_table_size(inode_to_port) == 0) {
        g_hash_table_unref(inode_to_port);
        return current_keys;
    }

    scopes = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    g_hash_table_iter_init(&iter, terminal_scopes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_hash_table_contains(scanned_ids, key))
            g_ptr_array_add(scopes, value);
    }

    proc_dir = opendir("/proc");
    if (!proc_dir) {
        g_ptr_array_unref(scopes);
        g_hash_table_unref(inode_to_port);
        return current_keys;
    }

    while ((ent = readdir(proc_dir)) != NULL) {
        char *end = NULL;
        long pid_long;
        pid_t pid;
        GPtrArray *matching_scopes;

        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;

        errno = 0;
        pid_long = strtol(ent->d_name, &end, 10);
        if (errno != 0 || !end || *end != '\0' || pid_long <= 0)
            continue;

        pid = (pid_t)pid_long;
        matching_scopes = g_ptr_array_new();
        for (guint i = 0; i < scopes->len; i++) {
            TerminalScope *scope = g_ptr_array_index(scopes, i);
            if (scope_matches_pid(scope, pid))
                g_ptr_array_add(matching_scopes, scope);
        }

        if (matching_scopes->len > 0)
            scan_pid_fds_for_scope(pid, matching_scopes, inode_to_port, current_keys);

        g_ptr_array_unref(matching_scopes);
    }

    closedir(proc_dir);
    g_ptr_array_unref(scopes);
    g_hash_table_unref(inode_to_port);
    return current_keys;
}

static GHashTable *
scanned_terminal_ids_for_tick(void)
{
    GHashTable *scanned_ids;
    GHashTableIter iter;
    gpointer key;
    gpointer value;
    gint64 now_us;
    gboolean include_background;

    scanned_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!terminal_scopes || g_hash_table_size(terminal_scopes) == 0)
        return scanned_ids;

    now_us = g_get_monotonic_time();
    include_background = (!window_active) ||
        (last_background_scan_us == 0) ||
        (now_us - last_background_scan_us >= SLOW_SCAN_INTERVAL_US);

    g_hash_table_iter_init(&iter, terminal_scopes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        TerminalScope *scope = value;
        gboolean scan_now;

        scan_now = FALSE;
        if (window_active && scope->workspace_idx >= 0 &&
            scope->workspace_idx == active_workspace_idx)
            scan_now = TRUE;
        else if (include_background)
            scan_now = TRUE;

        if (scan_now)
            g_hash_table_add(scanned_ids, g_strdup(scope->terminal_id));
    }

    if (include_background)
        last_background_scan_us = now_us;

    return scanned_ids;
}

static gboolean
scan_ports_tick(gpointer data)
{
    GHashTable *scanned_ids;
    GHashTable *current_keys;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    (void)data;

    scanned_ids = scanned_terminal_ids_for_tick();
    if (g_hash_table_size(scanned_ids) == 0) {
        g_hash_table_unref(scanned_ids);
        return G_SOURCE_CONTINUE;
    }

    current_keys = collect_current_port_keys(scanned_ids);

    g_hash_table_iter_init(&iter, current_keys);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *port_key = key;
        const char *sep = strrchr(port_key, '|');
        char *terminal_id;
        int port;

        if (!sep)
            continue;
        if (known_port_keys && g_hash_table_contains(known_port_keys, port_key))
            continue;

        terminal_id = g_strndup(port_key, sep - port_key);
        port = atoi(sep + 1);
        if (scan_callback && terminal_id[0] && port > 0)
            scan_callback(terminal_id, port, scan_user_data);
        g_free(terminal_id);
    }

    if (!known_port_keys)
        known_port_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_hash_table_foreach_remove(known_port_keys,
                                key_matches_any_scanned_terminal,
                                scanned_ids);

    g_hash_table_iter_init(&iter, current_keys);
    while (g_hash_table_iter_next(&iter, &key, &value))
        g_hash_table_add(known_port_keys, g_strdup(key));

    g_hash_table_unref(current_keys);
    g_hash_table_unref(scanned_ids);
    return G_SOURCE_CONTINUE;
}
#else
static gboolean
scan_ports_tick(gpointer data)
{
    (void)data;
    return G_SOURCE_CONTINUE;
}
#endif

void
port_scanner_set_callback(PortScannerCallback cb, gpointer user_data)
{
    scan_callback = cb;
    scan_user_data = user_data;
}

void
port_scanner_start(void)
{
    if (scan_timer_id != 0)
        return;

    if (!terminal_scopes)
        terminal_scopes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, terminal_scope_free);
    if (!known_port_keys)
        known_port_keys = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, NULL);

    scan_ports_tick(NULL);
    scan_timer_id = g_timeout_add_seconds(5, scan_ports_tick, NULL);
}

void
port_scanner_stop(void)
{
    if (scan_timer_id != 0) {
        g_source_remove(scan_timer_id);
        scan_timer_id = 0;
    }

    if (known_port_keys) {
        g_hash_table_unref(known_port_keys);
        known_port_keys = NULL;
    }

    if (terminal_scopes) {
        g_hash_table_unref(terminal_scopes);
        terminal_scopes = NULL;
    }

    last_background_scan_us = 0;
}

void
port_scanner_register_terminal(const char *terminal_id,
                               pid_t session_id,
                               const char *tty_name,
                               const char *tty_path,
                               int workspace_idx)
{
    TerminalScope *scope;

    if (!terminal_id || !terminal_id[0])
        return;

    if (!terminal_scopes)
        terminal_scopes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, terminal_scope_free);

    scope = g_hash_table_lookup(terminal_scopes, terminal_id);
    if (!scope) {
        scope = g_new0(TerminalScope, 1);
        scope->terminal_id = g_strdup(terminal_id);
        g_hash_table_insert(terminal_scopes, g_strdup(terminal_id), scope);
    }

    scope->session_id = session_id;
    g_free(scope->tty_name);
    scope->tty_name = g_strdup(tty_name ? tty_name : "");
    g_free(scope->tty_path);
    scope->tty_path = g_strdup(tty_path ? tty_path : "");
    scope->workspace_idx = workspace_idx;
}

void
port_scanner_unregister_terminal(const char *terminal_id)
{
    if (!terminal_scopes || !terminal_id || !terminal_id[0])
        return;

    g_hash_table_remove(terminal_scopes, terminal_id);
}

void
port_scanner_set_terminal_workspace(const char *terminal_id, int workspace_idx)
{
    TerminalScope *scope;

    if (!terminal_scopes || !terminal_id || !terminal_id[0])
        return;

    scope = g_hash_table_lookup(terminal_scopes, terminal_id);
    if (scope)
        scope->workspace_idx = workspace_idx;
}

void
port_scanner_set_active_workspace(int workspace_idx)
{
    active_workspace_idx = workspace_idx;
}

void
port_scanner_set_window_active(gboolean active)
{
    window_active = active;
}
