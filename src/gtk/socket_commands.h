#pragma once

#include <json-glib/json-glib.h>

void socket_commands_on_socket_command(const char  *command,
                                       JsonObject  *msg,
                                       JsonBuilder *response,
                                       gpointer     user_data);
