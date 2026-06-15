/*
 * socket_server.c - Unix domain socket for IPC
 *
 * Creates $XDG_RUNTIME_DIR/prettymux-<instance-id>.sock (falling back to
 * /tmp when XDG_RUNTIME_DIR is unset), accepts JSON commands.
 * Sends JSON responses back to the client.
 */

#include "socket_server.h"

#include "app_state.h"

#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ── State ────────────────────────────────────────────────────────── */

static GSocketService       *service = NULL;
static char                 *socket_path = NULL;
static SocketCommandCallback cmd_callback = NULL;
static gpointer              cmd_user_data = NULL;

static GQuark
socket_server_error_quark(void)
{
    return g_quark_from_static_string("prettymux-socket-server");
}

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

/* Per-user socket directory: $XDG_RUNTIME_DIR (mode 0700, owned by the user)
 * when available, else /tmp. Using XDG_RUNTIME_DIR keeps the socket out of the
 * world-writable /tmp, closing the predictable-path squatting / TOCTOU window.
 * The client path-builders (prettymux-open, prettymux_agent_cli) resolve the
 * directory the same way, and the server also exports the full path via
 * PRETTYMUX_SOCKET for clients running inside prettymux. */
static const char *
socket_runtime_dir(void)
{
    const char *dir = g_getenv("XDG_RUNTIME_DIR");
    return (dir && dir[0]) ? dir : "/tmp";
}

static char *
build_socket_path_for_instance(const char *instance_id)
{
    if (!instance_id_is_valid(instance_id))
        return NULL;
    return g_strdup_printf("%s/prettymux-%s.sock",
                           socket_runtime_dir(), instance_id);
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
write_all_bytes(int fd, const char *buf, gsize len, GError **error)
{
    const char *cursor = buf;
    gsize remaining = len;

    while (remaining > 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            g_set_error(error, socket_server_error_quark(), errno,
                        "write() failed: %s", g_strerror(errno));
            return FALSE;
        }
        if (written == 0) {
            g_set_error_literal(error, socket_server_error_quark(), EIO,
                                "write() wrote 0 bytes");
            return FALSE;
        }

        cursor += written;
        remaining -= (gsize)written;
    }

    return TRUE;
}

static gboolean
send_message_to_socket_path(const char *path,
                            const char *json_payload,
                            char **response_out,
                            GError **error)
{
    int fd = -1;
    struct sockaddr_un addr = {0};
    GString *response = NULL;
    char buffer[8192];
    ssize_t n = 0;

    if (response_out)
        *response_out = NULL;

    if (!path || !path[0] || !json_payload || !json_payload[0]) {
        g_set_error_literal(error, socket_server_error_quark(), EINVAL,
                            "missing socket path or payload");
        return FALSE;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_set_error(error, socket_server_error_quark(), errno,
                    "socket() failed: %s", g_strerror(errno));
        return FALSE;
    }

    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        g_set_error(error, socket_server_error_quark(), errno,
                    "connect(%s) failed: %s", path, g_strerror(errno));
        close(fd);
        return FALSE;
    }

    if (!write_all_bytes(fd, json_payload, strlen(json_payload), error)) {
        close(fd);
        return FALSE;
    }

    shutdown(fd, SHUT_WR);

    response = g_string_new("");
    while ((n = read(fd, buffer, sizeof(buffer))) > 0)
        g_string_append_len(response, buffer, n);

    if (n < 0) {
        g_set_error(error, socket_server_error_quark(), errno,
                    "read() failed: %s", g_strerror(errno));
        g_string_free(response, TRUE);
        close(fd);
        return FALSE;
    }

    close(fd);
    if (response_out)
        *response_out = g_string_free(response, FALSE);
    else
        g_string_free(response, TRUE);
    return TRUE;
}

static gboolean
copy_json_object_members(JsonBuilder *response,
                         JsonObject *source,
                         GError **error)
{
    GList *members;

    if (!response || !source) {
        g_set_error_literal(error, socket_server_error_quark(), EINVAL,
                            "missing response builder or source object");
        return FALSE;
    }

    members = json_object_get_members(source);
    for (GList *l = members; l; l = l->next) {
        const char *name = l->data;
        JsonNode *value_node = json_object_get_member(source, name);
        if (!name || !value_node)
            continue;

        json_builder_set_member_name(response, name);
        json_builder_add_value(response, json_node_copy(value_node));
    }
    g_list_free(members);
    return TRUE;
}

/* ── Client read callback ─────────────────────────────────────────── */

typedef struct {
    GSocketConnection *conn;
    GByteArray        *buf;
    gsize              data_len;  /* actual bytes received so far */
} ClientCtx;

static void
client_ctx_free(ClientCtx *ctx)
{
    if (ctx->conn)
        g_object_unref(ctx->conn);
    if (ctx->buf)
        g_byte_array_unref(ctx->buf);
    g_free(ctx);
}

static void
send_response_and_close(ClientCtx *ctx, JsonBuilder *resp_builder)
{
    /* Close the response object */
    json_builder_end_object(resp_builder);

    JsonNode *node = json_builder_get_root(resp_builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, node);
    gsize len = 0;
    char *json_str = json_generator_to_data(gen, &len);

    /* Write the response to the client */
    GOutputStream *out = g_io_stream_get_output_stream(
        G_IO_STREAM(ctx->conn));
    if (out && json_str) {
        g_output_stream_write_all(out, json_str, len, NULL, NULL, NULL);
        g_output_stream_write_all(out, "\n", 1, NULL, NULL, NULL);
    }

    g_free(json_str);
    json_node_unref(node);
    g_object_unref(gen);
    g_object_unref(resp_builder);
}

static void
on_client_read(GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
    ClientCtx *ctx = user_data;
    GError *error = NULL;

    gssize bytes_read = g_input_stream_read_finish(
        G_INPUT_STREAM(source), result, &error);

    if (bytes_read <= 0) {
        /* Connection closed or error — parse whatever we have */
        if (error)
            g_error_free(error);

        if (ctx->data_len > 0 && cmd_callback) {
            /* Null-terminate */
            ctx->buf->data[ctx->data_len] = '\0';

            JsonParser *parser = json_parser_new();
            if (json_parser_load_from_data(parser,
                                           (const char *)ctx->buf->data,
                                           (gssize)ctx->data_len, NULL)) {
                JsonNode *root = json_parser_get_root(parser);
                if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                    JsonObject *obj = json_node_get_object(root);
                    const char *command = json_object_get_string_member_with_default(
                        obj, "command", "");

                    /* Build response */
                    JsonBuilder *resp = json_builder_new();
                    json_builder_begin_object(resp);

                    cmd_callback(command, obj, resp, cmd_user_data);

                    send_response_and_close(ctx, resp);
                }
            }
            g_object_unref(parser);
        }

        client_ctx_free(ctx);
        return;
    }

    /* Got data — track how much real data we have */
    ctx->data_len += (gsize)bytes_read;

    /* Grow the buffer if needed and request more data */
    if (ctx->data_len + 4096 > ctx->buf->len)
        g_byte_array_set_size(ctx->buf, ctx->data_len + 4096);

    g_input_stream_read_async(
        G_INPUT_STREAM(source),
        ctx->buf->data + ctx->data_len,
        4096,
        G_PRIORITY_DEFAULT,
        NULL,
        on_client_read,
        ctx);
}

/* ── New connection handler ───────────────────────────────────────── */

static gboolean
on_incoming(GSocketService    *svc,
            GSocketConnection *conn,
            GObject           *source,
            gpointer           user_data)
{
    (void)svc;
    (void)source;
    (void)user_data;

    ClientCtx *ctx = g_new0(ClientCtx, 1);
    ctx->conn = g_object_ref(conn);
    ctx->buf = g_byte_array_sized_new(4096 + 1);  /* +1 for null terminator */
    g_byte_array_set_size(ctx->buf, 4096 + 1);
    ctx->data_len = 0;

    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    g_input_stream_read_async(
        input,
        ctx->buf->data,
        4096,
        G_PRIORITY_DEFAULT,
        NULL,
        on_client_read,
        ctx);

    return TRUE; /* We've handled the connection */
}

/* ── Public API ───────────────────────────────────────────────────── */

void
socket_server_set_callback(SocketCommandCallback cb, gpointer user_data)
{
    cmd_callback = cb;
    cmd_user_data = user_data;
}

gboolean
socket_server_route_command_to_instance(const char *instance_id,
                                        JsonObject *msg,
                                        JsonBuilder *response,
                                        GError **error)
{
    g_autofree char *target_socket_path = NULL;
    g_autofree char *request_payload = NULL;
    g_autofree char *response_payload = NULL;
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *request_root = NULL;
    JsonNode *response_root = NULL;

    if (!instance_id || !instance_id[0] || !msg || !response) {
        g_set_error_literal(error, socket_server_error_quark(), EINVAL,
                            "missing routing instance, command, or response");
        return FALSE;
    }

    target_socket_path = build_socket_path_for_instance(instance_id);
    if (!target_socket_path) {
        g_set_error(error, socket_server_error_quark(), EINVAL,
                    "invalid instance id '%s'", instance_id);
        return FALSE;
    }

    request_root = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(request_root, msg);
    generator = json_generator_new();
    json_generator_set_root(generator, request_root);
    request_payload = json_generator_to_data(generator, NULL);
    json_node_free(request_root);
    request_root = NULL;

    if (!send_message_to_socket_path(target_socket_path, request_payload,
                                     &response_payload, error))
        return FALSE;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, response_payload, -1, error))
        return FALSE;

    response_root = json_parser_get_root(parser);
    if (!response_root || !JSON_NODE_HOLDS_OBJECT(response_root)) {
        g_set_error_literal(error, socket_server_error_quark(), EINVAL,
                            "routed instance returned non-object JSON");
        return FALSE;
    }

    return copy_json_object_members(response,
                                    json_node_get_object(response_root),
                                    error);
}

const char *
socket_server_start(void)
{
    const char *instance_id;

    if (service)
        return socket_path;

    instance_id = app_state_get_instance_id();
    socket_path = build_socket_path_for_instance(instance_id);
    if (!socket_path) {
        fprintf(stderr, "socket_server: invalid instance id '%s'\n",
                instance_id ? instance_id : "(null)");
        return NULL;
    }

    /* Remove stale socket file only if it is not live. */
    if (g_file_test(socket_path, G_FILE_TEST_EXISTS)) {
        if (socket_path_is_connectable(socket_path)) {
            fprintf(stderr,
                    "socket_server: socket already in use for instance '%s': %s\n",
                    instance_id, socket_path);
            g_free(socket_path);
            socket_path = NULL;
            return NULL;
        }
        g_unlink(socket_path);
    }

    GError *error = NULL;
    GSocketAddress *addr = g_unix_socket_address_new(socket_path);

    service = g_socket_service_new();
    if (!g_socket_listener_add_address(
            G_SOCKET_LISTENER(service),
            addr,
            G_SOCKET_TYPE_STREAM,
            G_SOCKET_PROTOCOL_DEFAULT,
            NULL,   /* source_object */
            NULL,   /* effective_address */
            &error))
    {
        fprintf(stderr, "socket_server: failed to listen on %s: %s\n",
                socket_path, error->message);
        g_error_free(error);
        g_object_unref(addr);
        g_object_unref(service);
        service = NULL;
        g_free(socket_path);
        socket_path = NULL;
        return NULL;
    }
    g_object_unref(addr);

    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(service);

    /* Set env vars so child shells can find us */
    g_setenv("PRETTYMUX_SOCKET", socket_path, TRUE);
    g_setenv("PRETTYMUX_INSTANCE_ID", instance_id, TRUE);
    g_setenv("PRETTYMUX", "1", TRUE);

    return socket_path;
}

void
socket_server_stop(void)
{
    if (service) {
        g_socket_service_stop(service);
        g_object_unref(service);
        service = NULL;
    }

    if (socket_path) {
        g_unlink(socket_path);
        g_free(socket_path);
        socket_path = NULL;
    }
}

const char *
socket_server_get_path(void)
{
    return socket_server_get_instance_socket_path();
}

const char *
socket_server_get_instance_socket_path(void)
{
    return socket_path;
}
