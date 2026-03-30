#include "socket_server.h"

static SocketCommandCallback g_callback = NULL;
static gpointer g_callback_data = NULL;

void
socket_server_set_callback(SocketCommandCallback cb, gpointer user_data)
{
    g_callback = cb;
    g_callback_data = user_data;
    (void)g_callback;
    (void)g_callback_data;
}

const char *
socket_server_start(void)
{
    return NULL;
}

void
socket_server_stop(void)
{
}

const char *
socket_server_get_path(void)
{
    return NULL;
}
