#include "socket_server.h"

#include <errno.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    char path[256];
    const char *response_json;
    char *captured_payload;
    GThread *thread;
    GMutex mutex;
    GCond cond;
    gboolean ready;
} RouteServer;

static gboolean
write_all_fd(int fd, const char *buf, gsize len)
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

static gpointer
route_server_thread(gpointer data)
{
    RouteServer *server = data;
    struct sockaddr_un addr = {0};
    int listen_fd;

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
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
        GString *payload;
        char buffer[4096];
        ssize_t read_bytes;

        conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        payload = g_string_new("");
        while ((read_bytes = read(conn_fd, buffer, sizeof(buffer))) > 0)
            g_string_append_len(payload, buffer, read_bytes);

        if (payload->len > 0) {
            g_mutex_lock(&server->mutex);
            g_free(server->captured_payload);
            server->captured_payload = g_string_free(payload, FALSE);
            g_mutex_unlock(&server->mutex);

            write_all_fd(conn_fd, server->response_json,
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
route_server_start(RouteServer *server,
                   const char *instance_id,
                   const char *response_json)
{
    /* Match socket_server.c's build_socket_path_for_instance: the route
     * function under test resolves the socket dir from XDG_RUNTIME_DIR. */
    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0])
        rt = "/tmp";
    g_snprintf(server->path, sizeof(server->path), "%s/prettymux-%s.sock",
               rt, instance_id);
    server->response_json = response_json;
    server->captured_payload = NULL;
    server->thread = NULL;
    server->ready = FALSE;
    g_mutex_init(&server->mutex);
    g_cond_init(&server->cond);

    server->thread = g_thread_new("route-server", route_server_thread, server);

    g_mutex_lock(&server->mutex);
    while (!server->ready)
        g_cond_wait(&server->cond, &server->mutex);
    g_mutex_unlock(&server->mutex);
}

static void
route_server_stop(RouteServer *server)
{
    if (server->thread)
        g_thread_join(server->thread);
    server->thread = NULL;
}

static char *
make_instance_id(const char *tag)
{
    return g_strdup_printf("phase6-%s-%" G_GINT64_FORMAT, tag,
                           g_get_real_time());
}

static void
test_route_success(void)
{
    g_autofree char *instance_id = make_instance_id("route-ok");
    RouteServer server = {0};
    g_autoptr(GError) error = NULL;
    JsonObject *msg;
    JsonBuilder *response;
    JsonNode *root;
    JsonObject *obj;

    route_server_start(&server, instance_id,
                       "{\"status\":\"ok\",\"origin\":\"target\"}");

    msg = json_object_new();
    json_object_set_string_member(msg, "command", "workspace.list");
    json_object_set_string_member(msg, "instanceId", instance_id);

    response = json_builder_new();
    json_builder_begin_object(response);

    g_assert_true(socket_server_route_command_to_instance(instance_id, msg,
                                                          response, &error));
    g_assert_no_error(error);
    json_builder_end_object(response);

    root = json_builder_get_root(response);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
    obj = json_node_get_object(root);
    g_assert_cmpstr(
        json_object_get_string_member_with_default(obj, "status", ""), ==, "ok");
    g_assert_cmpstr(
        json_object_get_string_member_with_default(obj, "origin", ""), ==,
        "target");

    g_mutex_lock(&server.mutex);
    g_assert_nonnull(server.captured_payload);
    g_assert_nonnull(strstr(server.captured_payload,
                            "\"command\":\"workspace.list\""));
    g_assert_nonnull(strstr(server.captured_payload, "\"instanceId\":\""));
    g_mutex_unlock(&server.mutex);

    json_node_free(root);
    g_object_unref(response);
    json_object_unref(msg);

    route_server_stop(&server);
    g_mutex_clear(&server.mutex);
    g_cond_clear(&server.cond);
    g_free(server.captured_payload);
}

static void
test_route_unreachable_socket_fails(void)
{
    g_autofree char *instance_id = make_instance_id("route-missing");
    g_autoptr(GError) error = NULL;
    JsonBuilder *response = json_builder_new();
    JsonObject *msg = json_object_new();

    json_object_set_string_member(msg, "command", "workspace.list");
    json_builder_begin_object(response);

    g_assert_false(socket_server_route_command_to_instance(instance_id, msg,
                                                           response, &error));
    g_assert_nonnull(error);
    g_assert_cmpuint(error->domain, ==,
                     g_quark_from_static_string("prettymux-socket-server"));
    g_assert_nonnull(strstr(error->message, "connect("));

    json_builder_end_object(response);
    g_object_unref(response);
    json_object_unref(msg);
}

static void
test_route_invalid_instance_id_fails(void)
{
    g_autoptr(GError) error = NULL;
    JsonBuilder *response = json_builder_new();
    JsonObject *msg = json_object_new();

    json_object_set_string_member(msg, "command", "workspace.list");
    json_builder_begin_object(response);

    g_assert_false(socket_server_route_command_to_instance("bad/instance", msg,
                                                           response, &error));
    g_assert_error(error, g_quark_from_static_string("prettymux-socket-server"),
                   EINVAL);
    g_assert_nonnull(strstr(error->message, "invalid instance id"));

    json_builder_end_object(response);
    g_object_unref(response);
    json_object_unref(msg);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/socket-server/route/success", test_route_success);
    g_test_add_func("/socket-server/route/unreachable",
                    test_route_unreachable_socket_fails);
    g_test_add_func("/socket-server/route/invalid-instance-id",
                    test_route_invalid_instance_id_fails);

    return g_test_run();
}
