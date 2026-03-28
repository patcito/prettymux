/*
 * port_scanner.h - Periodic port scanning for dev servers
 *
 * Reads /proc/net/tcp and /proc/net/tcp6 every 5 seconds to detect
 * newly opened listening ports (1024-34999, excluding well-known services).
 * Calls a user-provided callback when new ports appear.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Callback signature: array of new port numbers, count, user_data */
typedef void (*PortScannerCallback)(const int *new_ports,
                                    int         new_port_count,
                                    const int  *all_ports,
                                    int         all_port_count,
                                    gpointer    user_data);

void port_scanner_start(void);
void port_scanner_stop(void);
void port_scanner_set_callback(PortScannerCallback cb, gpointer user_data);

G_END_DECLS
