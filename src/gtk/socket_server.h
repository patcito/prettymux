/*
 * socket_server.h - Unix domain socket for IPC
 *
 * Creates /tmp/prettymux-<PID>.sock, accepts JSON commands from
 * child processes (e.g. shell integration scripts, prettymux-open CLI).
 *
 * Supported commands:
 *   {"command": "browser.open", "url": "https://..."}
 *   {"command": "workspace.new", "name": "optional-name"}
 *   {"command": "workspace.list"}
 *   {"command": "workspace.switch", "index": 0}
 *   {"command": "tab.new"}
 */
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * Callback signature.
 *
 * @command: the "command" field from the JSON message.
 * @msg:     the full parsed JSON object (for reading extra fields).
 * @response: a JsonBuilder* already in object state.
 *            The callback may add members like "status", "data", etc.
 *            The caller will close the object and send it back.
 * @user_data: opaque user data.
 */
typedef void (*SocketCommandCallback)(const char  *command,
                                      JsonObject  *msg,
                                      JsonBuilder *response,
                                      gpointer     user_data);

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
