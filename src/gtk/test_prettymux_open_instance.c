#define _GNU_SOURCE

#include <errno.h>
#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define main prettymux_open_cli_entry
#include "prettymux-open.c"
#undef main

typedef struct {
    char path[256];
    int fd;
} PassiveSocket;

typedef struct {
    char path[256];
    const char *response_json;
    char *captured_payload;
    GThread *thread;
    GMutex mutex;
    GCond cond;
    gboolean ready;
} CaptureServer;

static gboolean
test_write_all_fd(int fd, const char *buf, gsize len)
{
    const char *cursor = buf;
    gsize remaining = len;

    while (remaining > 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        if (written == 0)
            return FALSE;
        cursor += written;
        remaining -= (gsize)written;
    }

    return TRUE;
}

static void
test_reset_targeting_state(void)
{
    g_cli_target_instance = NULL;
    g_cli_target_socket = NULL;
    set_socket_resolution_status(SOCKET_RESOLUTION_NONE, NULL);
    g_unsetenv("PRETTYMUX_SOCKET");
    g_unsetenv("PRETTYMUX_INSTANCE_ID");
}

static char *
test_make_instance_id(const char *tag)
{
    return g_strdup_printf("phase6-open-%s-%" G_GINT64_FORMAT, tag,
                           g_get_real_time());
}

static gboolean
passive_socket_start(PassiveSocket *sock, const char *instance_id)
{
    struct sockaddr_un addr = {0};

    if (!build_socket_path_for_instance(instance_id, sock->path,
                                        sizeof(sock->path)))
        return FALSE;

    sock->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock->fd < 0)
        return FALSE;

    unlink(sock->path);
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, sock->path, sizeof(addr.sun_path));
    if (bind(sock->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock->fd);
        sock->fd = -1;
        return FALSE;
    }

    if (listen(sock->fd, 64) < 0) {
        close(sock->fd);
        sock->fd = -1;
        unlink(sock->path);
        return FALSE;
    }
    return TRUE;
}

static void
passive_socket_stop(PassiveSocket *sock)
{
    if (sock->fd >= 0)
        close(sock->fd);
    if (sock->path[0])
        unlink(sock->path);
    sock->fd = -1;
    sock->path[0] = '\0';
}

static gpointer
capture_server_thread(gpointer data)
{
    CaptureServer *server = data;
    struct sockaddr_un addr = {0};
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    g_assert_cmpint(listen_fd, >=, 0);
    unlink(server->path);

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, server->path, sizeof(addr.sun_path));
    g_assert_cmpint(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)), ==,
                    0);
    g_assert_cmpint(listen(listen_fd, 16), ==, 0);

    g_mutex_lock(&server->mutex);
    server->ready = TRUE;
    g_cond_signal(&server->cond);
    g_mutex_unlock(&server->mutex);

    for (;;) {
        int conn_fd;
        GString *payload = g_string_new("");
        char buffer[4096];
        ssize_t read_bytes;

        conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            g_string_free(payload, TRUE);
            if (errno == EINTR)
                continue;
            break;
        }

        while ((read_bytes = read(conn_fd, buffer, sizeof(buffer))) > 0)
            g_string_append_len(payload, buffer, read_bytes);

        if (payload->len > 0) {
            g_mutex_lock(&server->mutex);
            g_free(server->captured_payload);
            server->captured_payload = g_string_free(payload, FALSE);
            g_mutex_unlock(&server->mutex);

            test_write_all_fd(conn_fd, server->response_json,
                              strlen(server->response_json));
            close(conn_fd);
            break;
        }

        g_string_free(payload, TRUE);
        close(conn_fd);
    }

    close(listen_fd);
    unlink(server->path);
    return NULL;
}

static void
capture_server_start(CaptureServer *server, const char *instance_id,
                     const char *response_json)
{
    build_socket_path_for_instance(instance_id, server->path,
                                   sizeof(server->path));
    server->response_json = response_json;
    server->captured_payload = NULL;
    server->thread = NULL;
    server->ready = FALSE;
    g_mutex_init(&server->mutex);
    g_cond_init(&server->cond);

    server->thread =
        g_thread_new("prettymux-open-capture", capture_server_thread, server);

    g_mutex_lock(&server->mutex);
    while (!server->ready)
        g_cond_wait(&server->cond, &server->mutex);
    g_mutex_unlock(&server->mutex);
}

static void
capture_server_stop(CaptureServer *server)
{
    if (server->thread)
        g_thread_join(server->thread);
    server->thread = NULL;

    g_mutex_clear(&server->mutex);
    g_cond_clear(&server->cond);
    g_free(server->captured_payload);
    server->captured_payload = NULL;
}

static int
run_prettymux_open_cli(const char *const *argv, int argc)
{
    return prettymux_open_cli_entry(argc, (char **)argv);
}

static void
test_default_resolution_prefers_deterministic_instance_order(void)
{
    g_autofree char *id_high =
        g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)g_get_real_time() + 2000);
    g_autofree char *id_low =
        g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)g_get_real_time() + 1000);
    PassiveSocket high_socket = { .fd = -1 };
    PassiveSocket low_socket = { .fd = -1 };
    const char *resolved;

    test_reset_targeting_state();
    g_assert_true(passive_socket_start(&high_socket, id_high));
    g_assert_true(passive_socket_start(&low_socket, id_low));

    resolved = find_socket();
    g_assert_nonnull(resolved);
    g_assert_cmpstr(resolved, ==, low_socket.path);

    passive_socket_stop(&high_socket);
    passive_socket_stop(&low_socket);
}

static void
test_env_instance_target_does_not_fallback(void)
{
    g_autofree char *id_old = test_make_instance_id("fallback-old");
    g_autofree char *id_new = test_make_instance_id("fallback-new");
    PassiveSocket old_socket = { .fd = -1 };
    PassiveSocket new_socket = { .fd = -1 };

    test_reset_targeting_state();
    g_assert_true(passive_socket_start(&old_socket, id_old));
    g_assert_true(passive_socket_start(&new_socket, id_new));

    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6-open-missing-instance", TRUE);
    g_assert_null(find_socket());
    g_assert_cmpint(g_socket_resolution_status, ==,
                    SOCKET_RESOLUTION_EXPLICIT_INSTANCE_UNREACHABLE);

    passive_socket_stop(&old_socket);
    passive_socket_stop(&new_socket);
}

static void
test_env_socket_target_does_not_fallback(void)
{
    g_autofree char *id_live = test_make_instance_id("fallback-live");
    PassiveSocket live_socket = { .fd = -1 };

    test_reset_targeting_state();
    g_assert_true(passive_socket_start(&live_socket, id_live));

    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-open-missing.sock", TRUE);
    g_assert_null(find_socket());
    g_assert_cmpint(g_socket_resolution_status, ==,
                    SOCKET_RESOLUTION_EXPLICIT_SOCKET_UNREACHABLE);

    passive_socket_stop(&live_socket);
}

static void
test_instance_flag_routes_to_target_instance(void)
{
    g_autofree char *target_id = test_make_instance_id("target");
    CaptureServer server = {0};
    int rc;

    test_reset_targeting_state();
    capture_server_start(&server, target_id, "{\"status\":\"ok\"}\n");

    g_cli_target_instance = target_id;
    rc = send_command("{\"command\":\"workspace.list\"}");
    g_assert_cmpint(rc, ==, 0);

    g_assert_nonnull(server.captured_payload);
    g_assert_nonnull(strstr(server.captured_payload, "\"instanceId\":\""));
    g_assert_nonnull(strstr(server.captured_payload, "\"command\":\"workspace.list\""));
    capture_server_stop(&server);
}

static void
test_set_workspace_status_command_serializes_payload(void)
{
    g_autofree char *target_id = test_make_instance_id("status-set");
    CaptureServer server = {0};
    const char *argv[] = {
        "prettymux-open",
        "--instance", NULL,
        "--set-workspace-status",
        "--id", "agent.main",
        "--provider", "codex",
        "--kind", "session",
        "--state", "running",
        "--summary", "indexing",
        "--detail", "status detail",
        "--attention",
        "--notify",
        "-w", "2",
    };
    int rc;

    argv[2] = target_id;
    test_reset_targeting_state();
    capture_server_start(&server, target_id, "{\"status\":\"ok\"}\n");

    rc = run_prettymux_open_cli(argv, G_N_ELEMENTS(argv));
    g_assert_cmpint(rc, ==, 0);

    g_assert_nonnull(server.captured_payload);
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"command\":\"workspace.status.set\""));
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"entryId\":\"agent.main\""));
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"provider\":\"codex\""));
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"workspace\":2"));
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"notify\":true"));

    capture_server_stop(&server);
}

static void
test_list_workspace_status_command_serializes_payload(void)
{
    g_autofree char *target_id = test_make_instance_id("status-list");
    CaptureServer server = {0};
    const char *argv[] = {
        "prettymux-open",
        "--instance", NULL,
        "--list-workspace-status",
        "-w", "4",
    };
    int rc;

    argv[2] = target_id;
    test_reset_targeting_state();
    capture_server_start(&server, target_id, "{\"status\":\"ok\"}\n");

    rc = run_prettymux_open_cli(argv, G_N_ELEMENTS(argv));
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(server.captured_payload);
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"command\":\"workspace.status.list\""));
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"workspace\":4"));

    capture_server_stop(&server);
}

static void
test_move_workspace_command_serializes_payload(void)
{
    g_autofree char *source_id = test_make_instance_id("move-ws-source");
    g_autofree char *target_id = test_make_instance_id("move-ws-target");
    CaptureServer server = {0};
    const char *argv[] = {
        "prettymux-open",
        "--instance", NULL,
        "--move-workspace",
        "--to-instance", NULL,
        "-w", "3",
    };
    int rc;

    argv[2] = source_id;
    argv[5] = target_id;

    test_reset_targeting_state();
    capture_server_start(&server, source_id, "{\"status\":\"ok\"}\n");

    rc = run_prettymux_open_cli(argv, G_N_ELEMENTS(argv));
    g_assert_cmpint(rc, ==, 0);
    g_assert_nonnull(server.captured_payload);
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"command\":\"workspace.move_to_instance\""));
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"workspace\":3"));
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"targetInstanceId\":\""));
    g_assert_nonnull(strstr(server.captured_payload, target_id));
    g_assert_nonnull(strstr(server.captured_payload, "\"instanceId\":\""));
    g_assert_nonnull(strstr(server.captured_payload, source_id));

    capture_server_stop(&server);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    /* Pin the socket dir to /tmp (the paths these tests construct) regardless
     * of the runner's XDG_RUNTIME_DIR; build_socket_path falls back to /tmp. */
    g_unsetenv("XDG_RUNTIME_DIR");

    g_test_add_func("/prettymux-open/instance/default-resolution-deterministic",
                    test_default_resolution_prefers_deterministic_instance_order);
    g_test_add_func("/prettymux-open/instance/env-instance-no-fallback",
                    test_env_instance_target_does_not_fallback);
    g_test_add_func("/prettymux-open/instance/env-socket-no-fallback",
                    test_env_socket_target_does_not_fallback);
    g_test_add_func("/prettymux-open/instance/flag-routes-target",
                    test_instance_flag_routes_to_target_instance);
    g_test_add_func("/prettymux-open/status/set-command-payload",
                    test_set_workspace_status_command_serializes_payload);
    g_test_add_func("/prettymux-open/status/list-command-payload",
                    test_list_workspace_status_command_serializes_payload);
    g_test_add_func("/prettymux-open/workspace/move-command-payload",
                    test_move_workspace_command_serializes_payload);

    return g_test_run();
}
