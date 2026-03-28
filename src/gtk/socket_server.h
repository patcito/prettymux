/*
 * socket_server.h - Unix domain socket for IPC
 *
 * Creates /tmp/prettymux-<PID>.sock, accepts JSON commands from
 * child processes (e.g. shell integration scripts).
 *
 * Supported commands:
 *   {"command": "browser.open", "url": "https://..."}
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Callback: command name + JSON object with the full message */
typedef void (*SocketCommandCallback)(const char *command,
                                      const char *url,
                                      gpointer    user_data);

/*
 * socket_server_start:
 *
 * Creates the socket and starts listening. Sets PRETTYMUX_SOCKET
 * and PRETTYMUX env vars. Returns the socket path (owned by the module).
 */
const char *socket_server_start(void);

/*
 * socket_server_stop:
 *
 * Closes the socket and removes the file.
 */
void socket_server_stop(void);

/*
 * socket_server_get_path:
 *
 * Returns the socket path, or NULL if not started.
 */
const char *socket_server_get_path(void);

void socket_server_set_callback(SocketCommandCallback cb, gpointer user_data);

G_END_DECLS
