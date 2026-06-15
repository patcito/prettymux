#define _GNU_SOURCE

#include <errno.h>
#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define main prettymux_agent_cli_entry
#include "prettymux_agent_cli.c"
#undef main

typedef struct {
    char path[256];
    int fd;
} PassiveSocket;

static void
test_reset_targeting_env(void)
{
    g_unsetenv("PRETTYMUX_SOCKET");
    g_unsetenv("PRETTYMUX_INSTANCE_ID");
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

    if (listen(sock->fd, 32) < 0) {
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

static void
test_default_resolution_prefers_deterministic_instance_order(void)
{
    g_autofree char *id_high =
        g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)g_get_real_time() + 2000);
    g_autofree char *id_low =
        g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)g_get_real_time() + 1000);
    g_autoptr(GError) error = NULL;
    g_autofree char *resolved = NULL;
    PassiveSocket high_socket = { .fd = -1 };
    PassiveSocket low_socket = { .fd = -1 };

    test_reset_targeting_env();
    g_assert_true(passive_socket_start(&high_socket, id_high));
    g_assert_true(passive_socket_start(&low_socket, id_low));

    resolved = find_socket_path(&error);
    g_assert_no_error(error);
    g_assert_nonnull(resolved);
    g_assert_cmpstr(resolved, ==, low_socket.path);

    passive_socket_stop(&high_socket);
    passive_socket_stop(&low_socket);
}

static void
test_env_instance_target_does_not_fallback(void)
{
    g_autofree char *id_live = g_strdup_printf("phase6-agent-live-%" G_GINT64_FORMAT,
                                               g_get_real_time());
    g_autoptr(GError) error = NULL;
    g_autofree char *resolved = NULL;
    PassiveSocket live_socket = { .fd = -1 };

    test_reset_targeting_env();
    g_assert_true(passive_socket_start(&live_socket, id_live));

    g_setenv("PRETTYMUX_INSTANCE_ID", "phase6-agent-missing-instance", TRUE);
    resolved = find_socket_path(&error);
    g_assert_null(resolved);
    g_assert_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                   ENOENT);
    g_assert_nonnull(
        strstr(error->message, "PRETTYMUX_INSTANCE_ID target is not reachable"));

    passive_socket_stop(&live_socket);
}

static void
test_env_socket_target_does_not_fallback(void)
{
    g_autofree char *id_live = g_strdup_printf("phase6-agent-socket-%" G_GINT64_FORMAT,
                                               g_get_real_time());
    g_autoptr(GError) error = NULL;
    g_autofree char *resolved = NULL;
    PassiveSocket live_socket = { .fd = -1 };

    test_reset_targeting_env();
    g_assert_true(passive_socket_start(&live_socket, id_live));

    g_setenv("PRETTYMUX_SOCKET", "/tmp/prettymux-agent-cli-missing.sock", TRUE);
    resolved = find_socket_path(&error);
    g_assert_null(resolved);
    g_assert_error(error, g_quark_from_static_string("prettymux-agent-cli"),
                   ENOENT);
    g_assert_nonnull(
        strstr(error->message, "PRETTYMUX_SOCKET target is not reachable"));

    passive_socket_stop(&live_socket);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    /* Pin the socket dir to /tmp (the paths these tests construct) regardless
     * of the runner's XDG_RUNTIME_DIR; build_socket_path falls back to /tmp. */
    g_unsetenv("XDG_RUNTIME_DIR");

    g_test_add_func("/prettymux-agent-cli/instance/default-resolution-deterministic",
                    test_default_resolution_prefers_deterministic_instance_order);
    g_test_add_func("/prettymux-agent-cli/instance/env-instance-no-fallback",
                    test_env_instance_target_does_not_fallback);
    g_test_add_func("/prettymux-agent-cli/instance/env-socket-no-fallback",
                    test_env_socket_target_does_not_fallback);

    return g_test_run();
}
