/*
 * socket_server.c - Unix domain socket for IPC
 *
 * Creates /tmp/prettymux-<PID>.sock, accepts JSON commands.
 */

#include "socket_server.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── State ────────────────────────────────────────────────────────── */

static GSocketService       *service = NULL;
static char                 *socket_path = NULL;
static SocketCommandCallback cmd_callback = NULL;
static gpointer              cmd_user_data = NULL;

/* ── Client read callback ─────────────────────────────────────────── */

typedef struct {
    GSocketConnection *conn;
    GByteArray        *buf;
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

        if (ctx->buf->len > 0 && cmd_callback) {
            /* Null-terminate */
            g_byte_array_append(ctx->buf, (const guint8 *)"\0", 1);

            JsonParser *parser = json_parser_new();
            if (json_parser_load_from_data(parser, (const char *)ctx->buf->data,
                                           -1, NULL)) {
                JsonNode *root = json_parser_get_root(parser);
                if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                    JsonObject *obj = json_node_get_object(root);
                    const char *command = json_object_get_string_member_with_default(
                        obj, "command", "");
                    const char *url = json_object_get_string_member_with_default(
                        obj, "url", "");
                    cmd_callback(command, url, cmd_user_data);
                }
            }
            g_object_unref(parser);
        }

        client_ctx_free(ctx);
        return;
    }

    /* Got data — append to buffer and keep reading */
    ctx->buf->len += (guint)bytes_read;

    /* Grow the buffer and request more data */
    gsize old_len = ctx->buf->len;
    g_byte_array_set_size(ctx->buf, old_len + 4096);

    g_input_stream_read_async(
        G_INPUT_STREAM(source),
        ctx->buf->data + old_len,
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
    ctx->buf = g_byte_array_sized_new(4096);
    g_byte_array_set_size(ctx->buf, 4096);

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

const char *
socket_server_start(void)
{
    if (service)
        return socket_path;

    /* Build path: /tmp/prettymux-<PID>.sock */
    socket_path = g_strdup_printf("/tmp/prettymux-%d.sock", (int)getpid());

    /* Remove stale socket file if it exists */
    g_unlink(socket_path);

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
    return socket_path;
}
